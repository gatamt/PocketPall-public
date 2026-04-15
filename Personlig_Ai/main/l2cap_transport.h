#ifndef L2CAP_TRANSPORT_H
#define L2CAP_TRANSPORT_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* L2CAP CoC PSM (dynamic range 0x0080-0x00FF) */
#define L2CAP_PSM               0x00A1

/* SDU/MTU for L2CAP CoC channel */
#define L2CAP_SDU_SIZE          2048
#define L2CAP_SDU_BUF_COUNT     8

/* Frame protocol: 8-byte header */
#define L2CAP_FRAME_HEADER_SIZE 8

/* Frame types */
#define FRAME_AUDIO_MIC         0x01    /* ESP32→iPhone: 16kHz PCM, 32 KB/s */
#define FRAME_AUDIO_SPK         0x02    /* iPhone→ESP32: ADPCM, ~12 KB/s */
#define FRAME_CMD_JSON          0x03    /* Bidirectional: JSON commands */
#define FRAME_DISPLAY_CMD       0x04    /* iPhone→ESP32: display commands */
#define FRAME_INPUT_EVENT       0x05    /* ESP32→iPhone: gesture/button/rotation */
#define FRAME_IMAGE_DATA        0x06    /* iPhone→ESP32: RGB565 image data */
#define FRAME_STATUS            0x07    /* Bidirectional: status/keepalive */
#define FRAME_PING              0x08    /* Keep-alive ping */
#define FRAME_PONG              0x09    /* Keep-alive pong */
#define FRAME_VIDEO_JPEG        0x0A    /* iPhone→ESP32: JPEG video frames */

/* Frame flags */
#define FRAME_FLAG_NONE         0x00
#define FRAME_FLAG_VIDEO_SOF    0x01    /* Start of JPEG frame */
#define FRAME_FLAG_VIDEO_EOF    0x02    /* End of JPEG frame */

/* Frame header layout:
 * [Type:1][Flags:1][SeqNum:2 LE][Length:4 LE][Payload:N]
 */

/* Callback types */
typedef void (*l2cap_audio_rx_cb_t)(const uint8_t *data, size_t len);
typedef void (*l2cap_command_rx_cb_t)(const char *json, size_t len);
typedef void (*l2cap_image_rx_cb_t)(const uint8_t *data, size_t len);
typedef void (*l2cap_display_cmd_rx_cb_t)(const uint8_t *data, size_t len);
typedef void (*l2cap_video_rx_cb_t)(const uint8_t *data, size_t len, uint8_t flags);
typedef void (*l2cap_connection_cb_t)(bool connected);

typedef struct {
    l2cap_audio_rx_cb_t      on_audio;          /* ADPCM audio from iPhone */
    l2cap_command_rx_cb_t    on_command;         /* JSON commands from iPhone */
    l2cap_image_rx_cb_t      on_image;           /* Image data from iPhone */
    l2cap_display_cmd_rx_cb_t on_display_cmd;    /* Display commands from iPhone */
    l2cap_video_rx_cb_t      on_video;           /* JPEG video frames from iPhone */
    l2cap_connection_cb_t    on_connection;       /* L2CAP channel open/close */
} l2cap_transport_callbacks_t;

/**
 * Initialize NimBLE stack with L2CAP CoC server and minimal GATT service.
 * Must be called once at startup.
 */
esp_err_t l2cap_transport_init(const l2cap_transport_callbacks_t *cbs);

/**
 * Start BLE advertising as "Pocket AI".
 */
esp_err_t l2cap_transport_start_advertising(void);

/**
 * Stop BLE advertising.
 */
esp_err_t l2cap_transport_stop_advertising(void);

/**
 * Check if L2CAP channel is connected.
 */
bool l2cap_transport_is_connected(void);

/**
 * Send mic audio data (16kHz PCM) to iPhone over L2CAP.
 */
esp_err_t l2cap_transport_send_audio(const int16_t *pcm, size_t bytes);

/**
 * Send JSON command to iPhone over L2CAP.
 */
esp_err_t l2cap_transport_send_command(const char *json);

/**
 * Send status JSON to iPhone over L2CAP.
 */
esp_err_t l2cap_transport_send_status(const char *json);

/**
 * Send input event (gesture/button/rotation) to iPhone over L2CAP.
 */
esp_err_t l2cap_transport_send_input_event(const uint8_t *data, size_t len);

#endif /* L2CAP_TRANSPORT_H */
