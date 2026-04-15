#include "wifi_transport.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

static const char *TAG = "wifi_tx";

/* ---- WiFi event bits ---- */
#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_FAIL_BIT         BIT1
#define WIFI_GOT_IP_BIT       BIT2
#define WIFI_SCAN_DONE_BIT    BIT3

/* ---- State ---- */
static wifi_transport_callbacks_t s_cbs = {0};
static EventGroupHandle_t s_wifi_event_group = NULL;
static volatile bool s_wifi_connected = false;
static volatile bool s_tcp_connected = false;
static int s_tcp_sock = -1;
static uint16_t s_tx_seq = 0;
static SemaphoreHandle_t s_tx_mutex = NULL;
static TaskHandle_t s_tcp_rx_task = NULL;
static TaskHandle_t s_reconnect_task = NULL;
static volatile bool s_should_reconnect = true;
static volatile bool s_tcp_shutdown = false;
static wifi_scan_done_cb_t s_scan_cb = NULL;

/* Current WiFi credentials */
static char s_current_ssid[33] = {0};
static char s_current_password[65] = {0};

/* Server address discovered via mDNS or fallback */
static char s_server_ip[16] = {0};
static uint16_t s_server_port = WIFI_TCP_PORT;

/* ---- RX frame accumulation buffer ---- */
#define RX_ACCUM_SIZE   (32 * 1024)
static uint8_t *s_rx_accum = NULL;
static size_t   s_rx_accum_len = 0;

/* ---- Frame encode/decode (same as L2CAP — transport-agnostic) ---- */

static void frame_encode_header(uint8_t *hdr, uint8_t type, uint8_t flags,
                                 uint16_t seq, uint32_t payload_len)
{
    hdr[0] = type;
    hdr[1] = flags;
    hdr[2] = (uint8_t)(seq & 0xFF);
    hdr[3] = (uint8_t)((seq >> 8) & 0xFF);
    hdr[4] = (uint8_t)(payload_len & 0xFF);
    hdr[5] = (uint8_t)((payload_len >> 8) & 0xFF);
    hdr[6] = (uint8_t)((payload_len >> 16) & 0xFF);
    hdr[7] = (uint8_t)((payload_len >> 24) & 0xFF);
}

static bool frame_decode_header(const uint8_t *data, size_t len,
                                 uint8_t *type, uint8_t *flags,
                                 uint16_t *seq, uint32_t *payload_len)
{
    if (len < TRANSPORT_FRAME_HEADER_SIZE) return false;

    *type = data[0];
    *flags = data[1];
    *seq = (uint16_t)(data[2] | (data[3] << 8));
    *payload_len = (uint32_t)(data[4] | (data[5] << 8) |
                               (data[6] << 16) | (data[7] << 24));
    return true;
}

/* ---- Send PONG over TCP ---- */
static void send_pong(uint16_t seq);

static void dispatch_frame(const uint8_t *data, size_t total_len)
{
    uint8_t type, flags;
    uint16_t seq;
    uint32_t payload_len;

    if (!frame_decode_header(data, total_len, &type, &flags, &seq, &payload_len)) {
        ESP_LOGW(TAG, "Frame too short: %u bytes", (unsigned)total_len);
        return;
    }

    const uint8_t *payload = data + TRANSPORT_FRAME_HEADER_SIZE;
    size_t available = total_len - TRANSPORT_FRAME_HEADER_SIZE;
    if (payload_len > available) {
        ESP_LOGW(TAG, "Frame truncated: header says %lu, available %u",
                 (unsigned long)payload_len, (unsigned)available);
        payload_len = (uint32_t)available;
    }

    switch (type) {
    case FRAME_AUDIO_SPK:
        if (s_cbs.on_audio && payload_len > 0)
            s_cbs.on_audio(payload, payload_len);
        break;
    case FRAME_CMD_JSON:
        if (s_cbs.on_command && payload_len > 0)
            s_cbs.on_command((const char *)payload, payload_len);
        break;
    case FRAME_IMAGE_DATA:
        if (s_cbs.on_image && payload_len > 0)
            s_cbs.on_image(payload, payload_len);
        break;
    case FRAME_DISPLAY_CMD:
        if (s_cbs.on_display_cmd && payload_len > 0)
            s_cbs.on_display_cmd(payload, payload_len);
        break;
    case FRAME_VIDEO_JPEG:
        if (s_cbs.on_video && payload_len > 0)
            s_cbs.on_video(payload, payload_len, flags);
        break;
    case FRAME_STATUS:
        if (s_cbs.on_command && payload_len > 0)
            s_cbs.on_command((const char *)payload, payload_len);
        break;
    case FRAME_PING:
        send_pong(seq);
        break;
    case FRAME_PONG:
        ESP_LOGD(TAG, "PONG received (seq=%u)", seq);
        break;
    default:
        ESP_LOGW(TAG, "Unknown frame type: 0x%02X", type);
        break;
    }
}

/* ---- RX stream reassembly (same logic as L2CAP) ---- */

static void process_rx_data(const uint8_t *data, size_t len)
{
    if (!s_rx_accum) return;

    if (s_rx_accum_len + len > RX_ACCUM_SIZE) {
        ESP_LOGW(TAG, "RX accum overflow (%u + %u > %u), discarding",
                 (unsigned)s_rx_accum_len, (unsigned)len, (unsigned)RX_ACCUM_SIZE);
        s_rx_accum_len = 0;
    }
    memcpy(s_rx_accum + s_rx_accum_len, data, len);
    s_rx_accum_len += len;

    while (s_rx_accum_len >= TRANSPORT_FRAME_HEADER_SIZE) {
        uint32_t payload_len = (uint32_t)(s_rx_accum[4] |
                                          (s_rx_accum[5] << 8) |
                                          (s_rx_accum[6] << 16) |
                                          (s_rx_accum[7] << 24));

        /* Sanity check: reject obviously invalid lengths */
        if (payload_len > RX_ACCUM_SIZE) {
            ESP_LOGW(TAG, "RX accum: invalid payload_len %lu, resync",
                     (unsigned long)payload_len);
            s_rx_accum_len--;
            memmove(s_rx_accum, s_rx_accum + 1, s_rx_accum_len);
            continue;
        }

        size_t frame_len = TRANSPORT_FRAME_HEADER_SIZE + payload_len;
        if (s_rx_accum_len < frame_len) {
            break; /* Wait for more data */
        }

        dispatch_frame(s_rx_accum, frame_len);

        size_t remaining = s_rx_accum_len - frame_len;
        if (remaining > 0) {
            memmove(s_rx_accum, s_rx_accum + frame_len, remaining);
        }
        s_rx_accum_len = remaining;
    }
}

/* ---- Send helper ---- */

static esp_err_t send_frame(uint8_t type, const uint8_t *payload, size_t payload_len)
{
    if (!s_tcp_connected || s_tcp_sock < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t hdr[TRANSPORT_FRAME_HEADER_SIZE];
    frame_encode_header(hdr, type, FRAME_FLAG_NONE, s_tx_seq++, (uint32_t)payload_len);

    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);

    /* Send header */
    int sent = send(s_tcp_sock, hdr, TRANSPORT_FRAME_HEADER_SIZE, 0);
    if (sent < 0) {
        ESP_LOGW(TAG, "TCP send header failed: errno %d", errno);
        xSemaphoreGive(s_tx_mutex);
        return ESP_FAIL;
    }

    /* Send payload */
    if (payload && payload_len > 0) {
        size_t remaining = payload_len;
        const uint8_t *ptr = payload;
        while (remaining > 0) {
            sent = send(s_tcp_sock, ptr, remaining, 0);
            if (sent < 0) {
                ESP_LOGW(TAG, "TCP send payload failed: errno %d", errno);
                xSemaphoreGive(s_tx_mutex);
                return ESP_FAIL;
            }
            ptr += sent;
            remaining -= sent;
        }
    }

    xSemaphoreGive(s_tx_mutex);
    return ESP_OK;
}

static void send_pong(uint16_t seq)
{
    if (!s_tcp_connected || s_tcp_sock < 0) return;

    uint8_t hdr[TRANSPORT_FRAME_HEADER_SIZE];
    frame_encode_header(hdr, FRAME_PONG, FRAME_FLAG_NONE, seq, 0);

    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    send(s_tcp_sock, hdr, TRANSPORT_FRAME_HEADER_SIZE, 0);
    xSemaphoreGive(s_tx_mutex);
}

/* ---- TCP close helper ---- */

static void close_tcp_connection(void)
{
    bool was_connected = s_tcp_connected;
    s_tcp_connected = false;

    if (s_tcp_sock >= 0) {
        shutdown(s_tcp_sock, SHUT_RDWR);
        close(s_tcp_sock);
        s_tcp_sock = -1;
    }

    s_rx_accum_len = 0;

    if (was_connected && s_cbs.on_connection) {
        s_cbs.on_connection(false);
    }
}

/* ---- mDNS discovery ---- */

static bool discover_server_mdns(void)
{
    ESP_LOGI(TAG, "Querying mDNS for %s.%s ...", WIFI_MDNS_SERVICE, WIFI_MDNS_PROTO);

    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(WIFI_MDNS_SERVICE, WIFI_MDNS_PROTO, 3000, 5, &results);
    if (err != ESP_OK || !results) {
        ESP_LOGW(TAG, "mDNS query failed or no results");
        return false;
    }

    /* Use first result */
    mdns_result_t *r = results;
    bool found = false;

    while (r) {
        if (r->addr && r->addr->addr.type == ESP_IPADDR_TYPE_V4) {
            esp_ip4_addr_t ip4 = r->addr->addr.u_addr.ip4;
            snprintf(s_server_ip, sizeof(s_server_ip), IPSTR, IP2STR(&ip4));
            s_server_port = r->port;
            ESP_LOGI(TAG, "mDNS found server: %s:%u", s_server_ip, s_server_port);
            found = true;
            break;
        }
        r = r->next;
    }

    mdns_query_results_free(results);
    return found;
}

/* ---- TCP connect ---- */

static esp_err_t tcp_connect_to_server(void)
{
    /* Try mDNS discovery first */
    if (!discover_server_mdns()) {
        /* Fallback: iPhone Personal Hotspot gateway IP */
        strncpy(s_server_ip, WIFI_HOTSPOT_GATEWAY_IP, sizeof(s_server_ip) - 1);
        s_server_port = WIFI_TCP_PORT;
        ESP_LOGI(TAG, "Using fallback: %s:%u", s_server_ip, s_server_port);
    }

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(s_server_port);
    inet_pton(AF_INET, s_server_ip, &dest_addr.sin_addr);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Socket creation failed: errno %d", errno);
        return ESP_FAIL;
    }

    /* Set TCP_NODELAY for low-latency audio */
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    /* Set receive timeout */
    struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    /* Set send timeout */
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    /* Set keepalive */
    int keepalive = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));

    ESP_LOGI(TAG, "Connecting TCP to %s:%u ...", s_server_ip, s_server_port);

    int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGW(TAG, "TCP connect failed: errno %d", errno);
        close(sock);
        return ESP_FAIL;
    }

    /* After connect, use longer receive timeout for data */
    timeout.tv_sec = 30;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    s_tcp_sock = sock;
    s_tcp_connected = true;
    s_tx_seq = 0;
    s_rx_accum_len = 0;

    ESP_LOGI(TAG, "TCP connected to %s:%u", s_server_ip, s_server_port);

    if (s_cbs.on_connection) {
        s_cbs.on_connection(true);
    }

    return ESP_OK;
}

/* ---- TCP RX task ---- */

static void tcp_rx_task(void *arg)
{
    (void)arg;
    uint8_t *rx_buf = malloc(8192);
    if (!rx_buf) {
        ESP_LOGE(TAG, "Failed to allocate TCP RX buffer");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP RX task started");

    while (!s_tcp_shutdown) {
        if (!s_tcp_connected || s_tcp_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int len = recv(s_tcp_sock, rx_buf, 8192, 0);
        if (len > 0) {
            process_rx_data(rx_buf, len);
        } else if (len == 0) {
            /* Connection closed by peer */
            ESP_LOGI(TAG, "TCP connection closed by peer");
            close_tcp_connection();
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Receive timeout — normal, just continue */
                continue;
            }
            ESP_LOGW(TAG, "TCP recv error: errno %d", errno);
            close_tcp_connection();
        }
    }

    free(rx_buf);
    ESP_LOGI(TAG, "TCP RX task exiting");
    vTaskDelete(NULL);
}

/* ---- Reconnect task ---- */
/* Only this task calls tcp_connect_to_server() — no concurrent connection attempts. */

static void reconnect_task_fn(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Reconnect task started");

    uint32_t backoff_ms = WIFI_RECONNECT_INTERVAL_MS;

    while (!s_tcp_shutdown) {
        /* Wait for notification OR timeout — allows immediate wake-up from event handler */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(backoff_ms));

        if (s_tcp_shutdown) break;

        /* If WiFi is connected but TCP is not, try to reconnect */
        if (s_wifi_connected && !s_tcp_connected && s_should_reconnect) {
            ESP_LOGI(TAG, "Attempting TCP reconnect...");
            if (tcp_connect_to_server() == ESP_OK) {
                backoff_ms = WIFI_RECONNECT_INTERVAL_MS;
            } else {
                backoff_ms = backoff_ms * 2;
                if (backoff_ms > 15000) backoff_ms = 15000;
            }
        } else if (s_tcp_connected) {
            /* Already connected, reset backoff */
            backoff_ms = WIFI_RECONNECT_INTERVAL_MS;
        }

        /* If WiFi disconnected and we have saved creds, try reconnect WiFi */
        if (!s_wifi_connected && !s_tcp_connected && s_should_reconnect && s_current_ssid[0] != '\0') {
            ESP_LOGI(TAG, "Attempting WiFi reconnect to '%s'...", s_current_ssid);
            wifi_config_t wifi_config = {0};
            strncpy((char *)wifi_config.sta.ssid, s_current_ssid, sizeof(wifi_config.sta.ssid) - 1);
            strncpy((char *)wifi_config.sta.password, s_current_password, sizeof(wifi_config.sta.password) - 1);
            wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

            esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
            esp_wifi_connect();

            /* Wait for connection result */
            EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdTRUE, pdFALSE,
                pdMS_TO_TICKS(10000));

            if (bits & WIFI_GOT_IP_BIT) {
                backoff_ms = WIFI_RECONNECT_INTERVAL_MS;
            }
        }
    }

    ESP_LOGI(TAG, "Reconnect task exiting");
    vTaskDelete(NULL);
}

/* ---- WiFi event handler ---- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WiFi STA started");
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "WiFi connected to AP");
            s_wifi_connected = true;
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "WiFi disconnected (reason=%d)", event->reason);
            s_wifi_connected = false;

            /* Close TCP if connected */
            if (s_tcp_connected) {
                close_tcp_connection();
            }

            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            break;
        }

        case WIFI_EVENT_SCAN_DONE:
            ESP_LOGI(TAG, "WiFi scan done");
            xEventGroupSetBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);

            if (s_scan_cb) {
                uint16_t ap_count = 0;
                esp_wifi_scan_get_ap_num(&ap_count);
                if (ap_count > WIFI_SCAN_MAX_APS) ap_count = WIFI_SCAN_MAX_APS;

                wifi_ap_record_t *ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
                wifi_scan_result_t *results = calloc(ap_count, sizeof(wifi_scan_result_t));

                if (ap_list && results) {
                    esp_wifi_scan_get_ap_records(&ap_count, ap_list);
                    for (uint16_t i = 0; i < ap_count; i++) {
                        strncpy(results[i].ssid, (const char *)ap_list[i].ssid, 32);
                        results[i].ssid[32] = '\0';
                        results[i].rssi = ap_list[i].rssi;
                        results[i].authmode = (uint8_t)ap_list[i].authmode;
                    }
                    s_scan_cb(results, ap_count);
                }

                free(ap_list);
                free(results);
                s_scan_cb = NULL;
            }
            break;

        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            xEventGroupSetBits(s_wifi_event_group, WIFI_GOT_IP_BIT);

            /* Wake reconnect task to try TCP connect immediately.
             * Don't call tcp_connect_to_server() here — it blocks for mDNS
             * and would run concurrently with the reconnect task. */
            if (!s_tcp_connected && s_reconnect_task) {
                xTaskNotifyGive(s_reconnect_task);
            }
        }
    }
}

/* ---- NVS credential management ---- */

esp_err_t wifi_transport_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "password", password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "WiFi credentials saved for '%s'", ssid);
    return err;
}

esp_err_t wifi_transport_load_credentials(char *ssid, size_t ssid_len,
                                           char *password, size_t password_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    err = nvs_get_str(handle, "ssid", ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, "password", password, &password_len);
    }

    nvs_close(handle);
    return err;
}

bool wifi_transport_has_saved_credentials(void)
{
    char ssid[33] = {0};
    size_t ssid_len = sizeof(ssid);
    char password[65] = {0};
    size_t password_len = sizeof(password);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return false;

    err = nvs_get_str(handle, "ssid", ssid, &ssid_len);
    nvs_close(handle);

    return (err == ESP_OK && ssid[0] != '\0');
}

esp_err_t wifi_transport_clear_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_erase_all(handle);
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

/* ---- Public API ---- */

esp_err_t wifi_transport_init(const wifi_transport_callbacks_t *cbs)
{
    if (cbs) {
        s_cbs = *cbs;
    }

    ESP_LOGI(TAG, "Initializing WiFi transport...");

    /* Create event group */
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        return ESP_FAIL;
    }

    /* Create TX mutex */
    s_tx_mutex = xSemaphoreCreateMutex();
    if (!s_tx_mutex) {
        ESP_LOGE(TAG, "Failed to create TX mutex");
        return ESP_FAIL;
    }

    /* Allocate RX accumulation buffer in PSRAM */
    s_rx_accum = heap_caps_malloc(RX_ACCUM_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rx_accum) {
        /* Fallback to internal RAM with smaller buffer */
        s_rx_accum = malloc(8192);
        if (!s_rx_accum) {
            ESP_LOGE(TAG, "Failed to allocate RX buffer");
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "RX buffer allocated in internal RAM (8KB)");
    }

    /* Initialize TCP/IP stack */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /* Initialize WiFi */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                    &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                    &wifi_event_handler, NULL, NULL));

    /* Set WiFi mode to STA */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Initialize mDNS */
    esp_err_t mdns_err = mdns_init();
    if (mdns_err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed: %s (will use fallback IP)", esp_err_to_name(mdns_err));
    } else {
        mdns_hostname_set(WIFI_DEVICE_NAME);
    }

    /* Start TCP RX task */
    s_tcp_shutdown = false;
    xTaskCreatePinnedToCore(
        tcp_rx_task, "tcp_rx",
        WIFI_TASK_STACK_SIZE, NULL,
        WIFI_TASK_PRIORITY, &s_tcp_rx_task,
        WIFI_TASK_CORE
    );

    /* Start reconnect task */
    xTaskCreatePinnedToCore(
        reconnect_task_fn, "wifi_recon",
        4096, NULL,
        WIFI_TASK_PRIORITY - 1, &s_reconnect_task,
        WIFI_TASK_CORE
    );

    ESP_LOGI(TAG, "WiFi transport initialized successfully");
    return ESP_OK;
}

esp_err_t wifi_transport_start_scan(wifi_scan_done_cb_t cb)
{
    s_scan_cb = cb;

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = { .min = 100, .max = 300 },
        },
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan start failed: %s", esp_err_to_name(err));
        s_scan_cb = NULL;
    }
    return err;
}

esp_err_t wifi_transport_connect_wifi(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    /* Store current credentials */
    strncpy(s_current_ssid, ssid, sizeof(s_current_ssid) - 1);
    s_current_ssid[sizeof(s_current_ssid) - 1] = '\0';
    if (password) {
        strncpy(s_current_password, password, sizeof(s_current_password) - 1);
        s_current_password[sizeof(s_current_password) - 1] = '\0';
    } else {
        s_current_password[0] = '\0';
    }

    s_should_reconnect = true;

    /* Configure WiFi */
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password && password[0] != '\0') {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    /* Clear previous event bits */
    xEventGroupClearBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_GOT_IP_BIT);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_LOGI(TAG, "Connecting to WiFi '%s'...", ssid);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Wait for connection result (blocking) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_GOT_IP_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(15000));

    if (bits & WIFI_GOT_IP_BIT) {
        ESP_LOGI(TAG, "WiFi connected and got IP");
        return ESP_OK;
    } else if (bits & WIFI_CONNECTED_BIT) {
        /* Connected but no IP yet — wait a bit more */
        bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_GOT_IP_BIT, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(10000));
        if (bits & WIFI_GOT_IP_BIT) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "WiFi connected but no IP received");
        return ESP_ERR_TIMEOUT;
    } else {
        ESP_LOGW(TAG, "WiFi connection failed");
        return ESP_FAIL;
    }
}

esp_err_t wifi_transport_disconnect(void)
{
    s_should_reconnect = false;
    close_tcp_connection();
    esp_wifi_disconnect();
    s_wifi_connected = false;
    s_current_ssid[0] = '\0';
    s_current_password[0] = '\0';
    return ESP_OK;
}

bool wifi_transport_is_connected(void)
{
    return s_tcp_connected;
}

bool wifi_transport_is_wifi_connected(void)
{
    return s_wifi_connected;
}

int8_t wifi_transport_get_rssi(void)
{
    if (!s_wifi_connected) return 0;

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

esp_err_t wifi_transport_send_audio(const int16_t *pcm, size_t bytes)
{
    if (!pcm || bytes == 0) return ESP_ERR_INVALID_ARG;
    return send_frame(FRAME_AUDIO_MIC, (const uint8_t *)pcm, bytes);
}

esp_err_t wifi_transport_send_command(const char *json)
{
    if (!json) return ESP_ERR_INVALID_ARG;
    return send_frame(FRAME_CMD_JSON, (const uint8_t *)json, strlen(json));
}

esp_err_t wifi_transport_send_status(const char *json)
{
    if (!json) return ESP_ERR_INVALID_ARG;
    return send_frame(FRAME_STATUS, (const uint8_t *)json, strlen(json));
}

esp_err_t wifi_transport_send_input_event(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    return send_frame(FRAME_INPUT_EVENT, data, len);
}
