#ifndef WIFI_TRANSPORT_H
#define WIFI_TRANSPORT_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Frame protocol: 8-byte header (same as L2CAP version — transport-agnostic) */
#define TRANSPORT_FRAME_HEADER_SIZE 8

/* Frame types (unchanged from L2CAP) */
#define FRAME_AUDIO_MIC         0x01    /* ESP32->iPhone: 16kHz PCM, 32 KB/s */
#define FRAME_AUDIO_SPK         0x02    /* iPhone->ESP32: ADPCM, ~12 KB/s */
#define FRAME_CMD_JSON          0x03    /* Bidirectional: JSON commands */
#define FRAME_DISPLAY_CMD       0x04    /* iPhone->ESP32: display commands */
#define FRAME_INPUT_EVENT       0x05    /* ESP32->iPhone: gesture/button/rotation */
#define FRAME_IMAGE_DATA        0x06    /* iPhone->ESP32: RGB565 image data */
#define FRAME_STATUS            0x07    /* Bidirectional: status/keepalive */
#define FRAME_PING              0x08    /* Keep-alive ping */
#define FRAME_PONG              0x09    /* Keep-alive pong */
#define FRAME_VIDEO_JPEG        0x0A    /* iPhone->ESP32: JPEG video frames */

/* Frame flags (unchanged) */
#define FRAME_FLAG_NONE         0x00
#define FRAME_FLAG_VIDEO_SOF    0x01    /* Start of JPEG frame */
#define FRAME_FLAG_VIDEO_EOF    0x02    /* End of JPEG frame */

/* Frame header layout:
 * [Type:1][Flags:1][SeqNum:2 LE][Length:4 LE][Payload:N]
 */

/* Callback types */
typedef void (*wifi_audio_rx_cb_t)(const uint8_t *data, size_t len);
typedef void (*wifi_command_rx_cb_t)(const char *json, size_t len);
typedef void (*wifi_image_rx_cb_t)(const uint8_t *data, size_t len);
typedef void (*wifi_display_cmd_rx_cb_t)(const uint8_t *data, size_t len);
typedef void (*wifi_video_rx_cb_t)(const uint8_t *data, size_t len, uint8_t flags);
typedef void (*wifi_connection_cb_t)(bool connected);

typedef struct {
    wifi_audio_rx_cb_t      on_audio;          /* ADPCM audio from iPhone */
    wifi_command_rx_cb_t    on_command;         /* JSON commands from iPhone */
    wifi_image_rx_cb_t      on_image;           /* Image data from iPhone */
    wifi_display_cmd_rx_cb_t on_display_cmd;    /* Display commands from iPhone */
    wifi_video_rx_cb_t      on_video;           /* JPEG video frames from iPhone */
    wifi_connection_cb_t    on_connection;       /* TCP connection open/close */
} wifi_transport_callbacks_t;

/* WiFi scan result */
typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;   /* wifi_auth_mode_t */
} wifi_scan_result_t;

typedef void (*wifi_scan_done_cb_t)(const wifi_scan_result_t *results, uint16_t count);

/**
 * Initialize WiFi STA mode and TCP client infrastructure.
 * Must be called once at startup.
 */
esp_err_t wifi_transport_init(const wifi_transport_callbacks_t *cbs);

/**
 * Start WiFi scanning for available networks.
 * Results delivered via callback.
 */
esp_err_t wifi_transport_start_scan(wifi_scan_done_cb_t cb);

/**
 * Connect to a WiFi access point (e.g., iPhone Personal Hotspot).
 * After WiFi connects and gets IP, automatically discovers and connects
 * to the PocketPall TCP server via mDNS.
 */
esp_err_t wifi_transport_connect_wifi(const char *ssid, const char *password);

/**
 * Disconnect from WiFi and close TCP socket.
 */
esp_err_t wifi_transport_disconnect(void);

/**
 * Check if TCP socket is connected to iPhone app.
 */
bool wifi_transport_is_connected(void);

/**
 * Check if WiFi STA is connected to access point.
 */
bool wifi_transport_is_wifi_connected(void);

/**
 * Get RSSI of current WiFi connection.
 * Returns 0 if not connected.
 */
int8_t wifi_transport_get_rssi(void);

/**
 * Send mic audio data (16kHz PCM) to iPhone over TCP.
 */
esp_err_t wifi_transport_send_audio(const int16_t *pcm, size_t bytes);

/**
 * Send JSON command to iPhone over TCP.
 */
esp_err_t wifi_transport_send_command(const char *json);

/**
 * Send status JSON to iPhone over TCP.
 */
esp_err_t wifi_transport_send_status(const char *json);

/**
 * Send input event (gesture/button/rotation) to iPhone over TCP.
 */
esp_err_t wifi_transport_send_input_event(const uint8_t *data, size_t len);

/* ---- NVS credential management ---- */

/**
 * Save WiFi credentials to NVS for auto-reconnect.
 */
esp_err_t wifi_transport_save_credentials(const char *ssid, const char *password);

/**
 * Load saved WiFi credentials from NVS.
 */
esp_err_t wifi_transport_load_credentials(char *ssid, size_t ssid_len,
                                           char *password, size_t password_len);

/**
 * Check if WiFi credentials are saved in NVS.
 */
bool wifi_transport_has_saved_credentials(void);

/**
 * Clear saved WiFi credentials from NVS.
 */
esp_err_t wifi_transport_clear_credentials(void);

#endif /* WIFI_TRANSPORT_H */
