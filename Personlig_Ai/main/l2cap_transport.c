#include "l2cap_transport.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_l2cap.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "l2cap";

/* ---- UUIDs ---- */

/* Pocket AI Service: 4F504149-424C-4500-0000-000000000001 (same as before for discovery) */
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x45, 0x4C, 0x42, 0x49, 0x41, 0x50, 0x4F
);

/* L2CAP PSM characteristic — advertises our PSM so iOS can read it */
static const ble_uuid128_t s_chr_psm_uuid = BLE_UUID128_INIT(
    0xA1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x45, 0x4C, 0x42, 0x49, 0x41, 0x50, 0x4F
);

/* ---- State ---- */

static l2cap_transport_callbacks_t s_cbs = {0};
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_ble_connected = false;
static volatile bool s_l2cap_connected = false;
static struct ble_l2cap_chan *s_l2cap_chan = NULL;
static uint16_t s_tx_seq = 0;

/* SDU buffer pool for L2CAP CoC */
static os_membuf_t s_sdu_coc_mem[OS_MEMPOOL_SIZE(L2CAP_SDU_BUF_COUNT, L2CAP_SDU_SIZE)];
static struct os_mempool s_sdu_coc_mempool;
static struct os_mbuf_pool s_sdu_coc_mbuf_pool;

/* ---- RX frame accumulation buffer ---- */
/* L2CAP CoC is a byte stream (iOS OutputStream may coalesce/split writes).
 * We accumulate received data and extract complete frames by header length. */
#define RX_ACCUM_SIZE (L2CAP_SDU_SIZE * 4)
static uint8_t s_rx_accum[RX_ACCUM_SIZE];
static size_t  s_rx_accum_len = 0;

/* ---- Frame encode/decode ---- */

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
    if (len < L2CAP_FRAME_HEADER_SIZE) return false;

    *type = data[0];
    *flags = data[1];
    *seq = (uint16_t)(data[2] | (data[3] << 8));
    *payload_len = (uint32_t)(data[4] | (data[5] << 8) |
                               (data[6] << 16) | (data[7] << 24));
    return true;
}

static void dispatch_frame(const uint8_t *data, size_t total_len)
{
    uint8_t type, flags;
    uint16_t seq;
    uint32_t payload_len;

    if (!frame_decode_header(data, total_len, &type, &flags, &seq, &payload_len)) {
        ESP_LOGW(TAG, "Frame too short: %u bytes", (unsigned)total_len);
        return;
    }

    const uint8_t *payload = data + L2CAP_FRAME_HEADER_SIZE;
    size_t available = total_len - L2CAP_FRAME_HEADER_SIZE;
    if (payload_len > available) {
        ESP_LOGW(TAG, "Frame truncated: header says %lu, available %u",
                 (unsigned long)payload_len, (unsigned)available);
        payload_len = (uint32_t)available;
    }

    switch (type) {
    case FRAME_AUDIO_SPK:
        if (s_cbs.on_audio && payload_len > 0) {
            s_cbs.on_audio(payload, payload_len);
        }
        break;
    case FRAME_CMD_JSON:
        if (s_cbs.on_command && payload_len > 0) {
            s_cbs.on_command((const char *)payload, payload_len);
        }
        break;
    case FRAME_IMAGE_DATA:
        if (s_cbs.on_image && payload_len > 0) {
            s_cbs.on_image(payload, payload_len);
        }
        break;
    case FRAME_DISPLAY_CMD:
        if (s_cbs.on_display_cmd && payload_len > 0) {
            s_cbs.on_display_cmd(payload, payload_len);
        }
        break;
    case FRAME_VIDEO_JPEG:
        if (s_cbs.on_video && payload_len > 0) {
            s_cbs.on_video(payload, payload_len, flags);
        }
        break;
    case FRAME_STATUS:
        if (s_cbs.on_command && payload_len > 0) {
            s_cbs.on_command((const char *)payload, payload_len);
        }
        break;
    case FRAME_PING:
        /* Respond with PONG */
        {
            uint8_t pong_hdr[L2CAP_FRAME_HEADER_SIZE];
            frame_encode_header(pong_hdr, FRAME_PONG, FRAME_FLAG_NONE, seq, 0);
            /* Best effort — ignore errors */
            if (s_l2cap_chan) {
                struct os_mbuf *om = os_mbuf_get_pkthdr(&s_sdu_coc_mbuf_pool, 0);
                if (om) {
                    if (os_mbuf_append(om, pong_hdr, L2CAP_FRAME_HEADER_SIZE) == 0) {
                        ble_l2cap_send(s_l2cap_chan, om);
                    } else {
                        os_mbuf_free_chain(om);
                    }
                }
            }
        }
        break;
    case FRAME_PONG:
        ESP_LOGD(TAG, "PONG received (seq=%u)", seq);
        break;
    default:
        ESP_LOGW(TAG, "Unknown frame type: 0x%02X", type);
        break;
    }
}

/* ---- RX stream reassembly ---- */

static void process_rx_data(const uint8_t *data, size_t len)
{
    /* Append incoming SDU data to accumulation buffer */
    if (s_rx_accum_len + len > RX_ACCUM_SIZE) {
        ESP_LOGW(TAG, "RX accum overflow (%u + %u > %u), discarding",
                 (unsigned)s_rx_accum_len, (unsigned)len, (unsigned)RX_ACCUM_SIZE);
        s_rx_accum_len = 0;
    }
    memcpy(s_rx_accum + s_rx_accum_len, data, len);
    s_rx_accum_len += len;

    /* Extract complete frames */
    while (s_rx_accum_len >= L2CAP_FRAME_HEADER_SIZE) {
        /* Peek at payload length from header bytes [4..7] */
        uint32_t payload_len = (uint32_t)(s_rx_accum[4] |
                                          (s_rx_accum[5] << 8) |
                                          (s_rx_accum[6] << 16) |
                                          (s_rx_accum[7] << 24));

        /* Sanity check: reject obviously invalid lengths */
        if (payload_len > L2CAP_SDU_SIZE) {
            ESP_LOGW(TAG, "RX accum: invalid payload_len %lu, resync",
                     (unsigned long)payload_len);
            /* Discard one byte and try to resync */
            s_rx_accum_len--;
            memmove(s_rx_accum, s_rx_accum + 1, s_rx_accum_len);
            continue;
        }

        size_t frame_len = L2CAP_FRAME_HEADER_SIZE + payload_len;
        if (s_rx_accum_len < frame_len) {
            break; /* Wait for more data */
        }

        /* Dispatch complete frame */
        dispatch_frame(s_rx_accum, frame_len);

        /* Remove consumed frame from accumulator */
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
    if (!s_l2cap_connected || !s_l2cap_chan) {
        return ESP_ERR_INVALID_STATE;
    }

    struct os_mbuf *om = os_mbuf_get_pkthdr(&s_sdu_coc_mbuf_pool, 0);
    if (!om) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t hdr[L2CAP_FRAME_HEADER_SIZE];
    frame_encode_header(hdr, type, FRAME_FLAG_NONE, s_tx_seq++, (uint32_t)payload_len);

    int rc = os_mbuf_append(om, hdr, L2CAP_FRAME_HEADER_SIZE);
    if (rc != 0) {
        os_mbuf_free_chain(om);
        return ESP_FAIL;
    }

    if (payload && payload_len > 0) {
        rc = os_mbuf_append(om, payload, payload_len);
        if (rc != 0) {
            os_mbuf_free_chain(om);
            return ESP_FAIL;
        }
    }

    rc = ble_l2cap_send(s_l2cap_chan, om);
    if (rc != 0) {
        if (rc == BLE_HS_ESTALLED || rc == BLE_HS_ENOMEM) {
            /* Channel congested — back off briefly */
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        /* om is freed by ble_l2cap_send on error unless BLE_HS_ESTALLED */
        if (rc == BLE_HS_ESTALLED) {
            os_mbuf_free_chain(om);
        }
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* ---- L2CAP CoC Event Handler ---- */

static int l2cap_event_cb(struct ble_l2cap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_L2CAP_EVENT_COC_CONNECTED:
        if (event->connect.status == 0) {
            s_l2cap_chan = event->connect.chan;
            s_l2cap_connected = true;
            s_tx_seq = 0;
            s_rx_accum_len = 0;
            {
                struct ble_l2cap_chan_info chan_info;
                if (ble_l2cap_get_chan_info(s_l2cap_chan, &chan_info) == 0) {
                    ESP_LOGI(TAG, "L2CAP CoC channel connected (our_mtu=%d, peer_mtu=%d)",
                             chan_info.our_l2cap_mtu, chan_info.peer_l2cap_mtu);
                } else {
                    ESP_LOGI(TAG, "L2CAP CoC channel connected");
                }
            }
            if (s_cbs.on_connection) {
                s_cbs.on_connection(true);
            }
        } else {
            ESP_LOGW(TAG, "L2CAP CoC connect failed: status=%d", event->connect.status);
        }
        return 0;

    case BLE_L2CAP_EVENT_COC_DISCONNECTED:
        ESP_LOGI(TAG, "L2CAP CoC channel disconnected");
        s_l2cap_chan = NULL;
        s_l2cap_connected = false;
        s_rx_accum_len = 0;
        if (s_cbs.on_connection) {
            s_cbs.on_connection(false);
        }
        return 0;

    case BLE_L2CAP_EVENT_COC_ACCEPT: {
        /* Accept incoming L2CAP CoC connection — provide RX SDU buffer */
        struct os_mbuf *sdu_rx = os_mbuf_get_pkthdr(&s_sdu_coc_mbuf_pool, 0);
        if (!sdu_rx) {
            ESP_LOGE(TAG, "No SDU buffer for L2CAP accept");
            return BLE_HS_ENOMEM;
        }
        ble_l2cap_recv_ready(event->accept.chan, sdu_rx);
        ESP_LOGI(TAG, "L2CAP CoC accept (peer_sdu=%u)", event->accept.peer_sdu_size);
        return 0;
    }

    case BLE_L2CAP_EVENT_COC_DATA_RECEIVED: {
        struct os_mbuf *rxom = event->receive.sdu_rx;
        if (rxom) {
            uint16_t len = OS_MBUF_PKTLEN(rxom);
            if (len > 0 && len <= L2CAP_SDU_SIZE) {
                uint8_t *buf = malloc(len);
                if (buf) {
                    os_mbuf_copydata(rxom, 0, len, buf);
                    process_rx_data(buf, len);
                    free(buf);
                } else {
                    ESP_LOGW(TAG, "No heap for L2CAP RX (%u bytes)", len);
                }
            }

            /* Free the consumed SDU mbuf */
            os_mbuf_free_chain(rxom);

            /* Provide fresh buffer for next receive */
            struct os_mbuf *next_sdu = os_mbuf_get_pkthdr(&s_sdu_coc_mbuf_pool, 0);
            if (next_sdu) {
                ble_l2cap_recv_ready(event->receive.chan, next_sdu);
            } else {
                ESP_LOGW(TAG, "No SDU buffer for next L2CAP receive");
            }
        }
        return 0;
    }

    case BLE_L2CAP_EVENT_COC_TX_UNSTALLED:
        ESP_LOGD(TAG, "L2CAP TX unstalled");
        return 0;

    default:
        return 0;
    }
}

/* ---- GATT: Minimal service for discovery + PSM characteristic ---- */

static int psm_chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint16_t psm = L2CAP_PSM;
        int rc = os_mbuf_append(ctxt->om, &psm, sizeof(psm));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            /* PSM characteristic: iOS reads this to know which PSM to connect to */
            {
                .uuid = &s_chr_psm_uuid.u,
                .access_cb = psm_chr_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ---- GAP Event Handler ---- */

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_ble_connected = true;
            ESP_LOGI(TAG, "BLE connected (handle=%d)", s_conn_handle);

            /* Request 2M PHY for higher throughput */
            ble_gap_set_prefered_le_phy(s_conn_handle,
                BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK,
                BLE_GAP_LE_PHY_CODED_ANY);

            /* Request fast connection parameters */
            struct ble_gap_upd_params params = {
                .itvl_min = BLE_CONN_INTERVAL_MIN,
                .itvl_max = BLE_CONN_INTERVAL_MAX,
                .latency = BLE_SLAVE_LATENCY,
                .supervision_timeout = BLE_SUPERVISION_TIMEOUT,
                .min_ce_len = 0,
                .max_ce_len = 0,
            };
            ble_gap_update_params(s_conn_handle, &params);

            /* L2CAP CoC server already created in ble_on_sync — nothing else needed */
        } else {
            ESP_LOGW(TAG, "BLE connection failed (status=%d)", event->connect.status);
            s_ble_connected = false;
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            l2cap_transport_start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected (reason=%d)", event->disconnect.reason);
        s_ble_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

        if (s_l2cap_connected) {
            s_l2cap_chan = NULL;
            s_l2cap_connected = false;
            if (s_cbs.on_connection) {
                s_cbs.on_connection(false);
            }
        }

        l2cap_transport_start_advertising();
        break;

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "Connection params updated");
        break;

    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        ESP_LOGI(TAG, "PHY updated: TX=%d, RX=%d",
                 event->phy_updated.tx_phy, event->phy_updated.rx_phy);
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGD(TAG, "Advertising complete");
        break;

    default:
        break;
    }

    return 0;
}

/* ---- NimBLE Host Task ---- */

static void ble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_on_sync(void)
{
    ESP_LOGI(TAG, "BLE host synced");

    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to ensure address: %d", rc);
        return;
    }

    /* Create L2CAP CoC server listening on our PSM */
    rc = ble_l2cap_create_server(L2CAP_PSM, L2CAP_SDU_SIZE, l2cap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "L2CAP server create failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "L2CAP CoC server listening on PSM 0x%04X (SDU=%d)",
                 L2CAP_PSM, L2CAP_SDU_SIZE);
    }

    l2cap_transport_start_advertising();
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset (reason=%d)", reason);
}

/* ---- Public API ---- */

esp_err_t l2cap_transport_init(const l2cap_transport_callbacks_t *cbs)
{
    if (cbs) {
        s_cbs = *cbs;
    }

    ESP_LOGI(TAG, "Initializing L2CAP transport...");

    /* Initialize SDU memory pool for L2CAP CoC */
    int rc = os_mempool_init(&s_sdu_coc_mempool, L2CAP_SDU_BUF_COUNT,
                              L2CAP_SDU_SIZE, s_sdu_coc_mem, "l2cap_sdu");
    if (rc != 0) {
        ESP_LOGE(TAG, "SDU mempool init failed: %d", rc);
        return ESP_FAIL;
    }

    rc = os_mbuf_pool_init(&s_sdu_coc_mbuf_pool, &s_sdu_coc_mempool,
                            L2CAP_SDU_SIZE, L2CAP_SDU_BUF_COUNT);
    if (rc != 0) {
        ESP_LOGE(TAG, "SDU mbuf pool init failed: %d", rc);
        return ESP_FAIL;
    }

    /* Initialize NimBLE */
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init() failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure NimBLE host */
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;

    /* Set preferred MTU for GATT (still needed for service discovery) */
    rc = ble_att_set_preferred_mtu(BLE_MTU_SIZE);
    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to set preferred MTU: %d", rc);
    }

    /* Initialize GAP and GATT services */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    /* Set device name */
    rc = ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to set device name: %d", rc);
    }

    /* Register minimal GATT service (for discovery + PSM read) */
    rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }

    /* Start NimBLE host task */
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "L2CAP transport initialized successfully");
    return ESP_OK;
}

esp_err_t l2cap_transport_start_advertising(void)
{
    struct ble_gap_adv_params adv_params = {0};
    struct ble_hs_adv_fields fields = {0};
    struct ble_hs_adv_fields rsp_fields = {0};

    /* Advertising data */
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.uuids128 = (ble_uuid128_t[]){ s_svc_uuid };
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return ESP_FAIL;
    }

    /* Scan response data (device name) */
    rsp_fields.name = (uint8_t *)BLE_DEVICE_NAME;
    rsp_fields.name_len = strlen(BLE_DEVICE_NAME);
    rsp_fields.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields failed: %d", rc);
        return ESP_FAIL;
    }

    /* Advertising parameters */
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = 0x0020;  /* 20ms */
    adv_params.itvl_max = 0x0040;  /* 40ms */

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                            &adv_params, ble_gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BLE advertising started as '%s'", BLE_DEVICE_NAME);
    return ESP_OK;
}

esp_err_t l2cap_transport_stop_advertising(void)
{
    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_adv_stop failed: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool l2cap_transport_is_connected(void)
{
    return s_l2cap_connected;
}

esp_err_t l2cap_transport_send_audio(const int16_t *pcm, size_t bytes)
{
    if (!pcm || bytes == 0) return ESP_ERR_INVALID_ARG;
    return send_frame(FRAME_AUDIO_MIC, (const uint8_t *)pcm, bytes);
}

esp_err_t l2cap_transport_send_command(const char *json)
{
    if (!json) return ESP_ERR_INVALID_ARG;
    return send_frame(FRAME_CMD_JSON, (const uint8_t *)json, strlen(json));
}

esp_err_t l2cap_transport_send_status(const char *json)
{
    if (!json) return ESP_ERR_INVALID_ARG;
    return send_frame(FRAME_STATUS, (const uint8_t *)json, strlen(json));
}

esp_err_t l2cap_transport_send_input_event(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    return send_frame(FRAME_INPUT_EVENT, data, len);
}
