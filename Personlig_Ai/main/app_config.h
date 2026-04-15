#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "driver/gpio.h"

/* ============================
 * I2C Bus (shared by all I2C devices)
 * ============================ */
#define I2C_MASTER_NUM          I2C_NUM_0
#define I2C_MASTER_SDA          GPIO_NUM_15
#define I2C_MASTER_SCL          GPIO_NUM_14
#define I2C_MASTER_FREQ_HZ     100000

/* ============================
 * Power Management
 * ============================ */
#define AXP2101_I2C_ADDR        0x34
#define TCA9554_I2C_ADDR        0x20

/* ============================
 * Display: SH8601 AMOLED via QSPI
 * ============================ */
#define LCD_HOST                SPI2_HOST
#define LCD_QSPI_CLK            GPIO_NUM_11
#define LCD_QSPI_D0             GPIO_NUM_4
#define LCD_QSPI_D1             GPIO_NUM_5
#define LCD_QSPI_D2             GPIO_NUM_6
#define LCD_QSPI_D3             GPIO_NUM_7
#define LCD_QSPI_CS             GPIO_NUM_12
#define LCD_PCLK_HZ             (30 * 1000 * 1000)
#define LCD_H_RES               368
#define LCD_V_RES               448
#define LCD_BPP                 16
#define LCD_MIRROR_X            0
#define LCD_MIRROR_Y            0

#define LCD_BUF_LINES           45
#define LCD_BUF_SIZE            (LCD_H_RES * LCD_BUF_LINES * sizeof(uint16_t))
#define LCD_TRANS_LINES         16

/* ============================
 * Touch: FT3168 (FT5x06 compatible)
 * ============================ */
#define TOUCH_I2C_ADDR          0x38
#define TOUCH_INT_PIN           GPIO_NUM_21
#define TOUCH_RESET_EXIO_PIN    2

/* ============================
 * IMU: QMI8658 6-axis
 * ============================ */
#define QMI8658_I2C_ADDR        0x6B
#define QMI8658_WHOAMI_VAL      0x05

/* Auto-rotation tuning */
#define IMU_ROTATE_POLL_MS          100     /* Faster poll for smoother filter */
#define IMU_ROTATE_STABLE_SAMPLES   3       /* 3 × 100ms = 300ms reaction time */
#define IMU_ROTATE_MIN_TILT_G       0.45f
#define IMU_ROTATE_MAX_FLAT_Z_G     0.82f
/* Final correction for this board mounting:
 * 1) counter-rotate UI against device motion
 * 2) offset one 90-degree step to compensate panel/IMU mounting shift
 */
#define IMU_ROTATE_COUNTER_UI       1
#define IMU_ROTATE_OFFSET_STEPS     1

/* Smooth rotation animation (fade-through-black) */
#define IMU_ROTATE_FADE_OUT_MS      150     /* Fade to black duration */
#define IMU_ROTATE_FADE_IN_MS       200     /* Fade from black (slightly slower = elegant) */
#define IMU_ROTATE_EMA_ALPHA        0.25f   /* Accelerometer low-pass filter (0-1, lower = smoother) */

/* ============================
 * Audio: ES8311 codec
 * ============================ */
/* esp_codec_dev i2c ctrl expects 8-bit I2C address (7-bit 0x18 => 0x30) */
#define ES8311_I2C_ADDR         0x30
#define I2S_NUM                 I2S_NUM_0
#define I2S_MCLK_PIN            GPIO_NUM_16
#define I2S_BCLK_PIN            GPIO_NUM_9
#define I2S_WS_PIN              GPIO_NUM_45
#define I2S_DOUT_PIN            GPIO_NUM_8
#define I2S_DIN_PIN             GPIO_NUM_10
#define PA_ENABLE_PIN           GPIO_NUM_46

#define AUDIO_SAMPLE_RATE_REC   16000
#define AUDIO_SAMPLE_RATE_PLAY  24000
#define AUDIO_BIT_WIDTH         16
#define AUDIO_CHANNELS          1
#define AUDIO_REC_INPUT_CHANNELS 2
#define AUDIO_MCLK_MULTIPLE     256
#define AUDIO_INPUT_GAIN_DB     32.0f

#define VAD_ENERGY_THRESHOLD    60
#define VAD_SILENCE_TIMEOUT_MS  1500
#define VAD_MIN_SPEECH_MS       120
#define VAD_LIVE_SILENCE_DEBOUNCE_MS  300

#define VAD_STREAM_MIN_SPEECH_MS       120
#define VAD_STREAM_START_MARGIN        90
#define VAD_STREAM_STOP_MARGIN         55
#define VAD_STREAM_START_FLOOR_MARGIN  45
#define VAD_STREAM_START_THRESHOLD_MAX 5000
#define VAD_STREAM_REOPEN_BLOCK_MS     120
#define VAD_STREAM_PREROLL_MS          240
#define VAD_STREAM_PREROLL_MAX_CHUNKS  20

#define AUDIO_REC_CHUNK_SAMPLES 240
#define AUDIO_REC_CHUNK_BYTES   (AUDIO_REC_CHUNK_SAMPLES * sizeof(int16_t))
#define AUDIO_REC_MAX_SECONDS   10
#define AUDIO_REC_BUF_SIZE      (AUDIO_SAMPLE_RATE_REC * 2 * AUDIO_REC_MAX_SECONDS)

#define AUDIO_PLAY_RINGBUF_SIZE (AUDIO_SAMPLE_RATE_PLAY * 2 * 10)  /* 10 seconds */
#define MIC_AUDIO_SEND_RETRY_COUNT      2
#define MIC_RESUME_HOLDOFF_AFTER_RESPONSE_MS 0
#define MIC_RESUME_HOLDOFF_AFTER_IMAGE_MS    700
#define MIC_STARTUP_PASSTHROUGH_MS      300
#define AI_RESPONSE_FALLBACK_DONE_MS    6000

/* ============================
 * WiFi Transport (TCP over iPhone Personal Hotspot)
 * ============================ */
#define WIFI_DEVICE_NAME            "Pocket AI"
#define WIFI_TCP_PORT               5050
#define WIFI_MDNS_SERVICE           "_pocketai"
#define WIFI_MDNS_PROTO             "_tcp"
#define WIFI_HOTSPOT_GATEWAY_IP     "172.20.10.1"
#define WIFI_AUDIO_CHUNK_SIZE       480     /* 480 bytes = 240 samples = 15ms at 16kHz */
#define WIFI_IMAGE_CHUNK_SIZE       4096    /* 4KB chunks for image transfer */
#define WIFI_VIDEO_CHUNK_SIZE       4096    /* TCP can handle larger chunks than BLE */
#define WIFI_TASK_STACK_SIZE        (6 * 1024)
#define WIFI_TASK_PRIORITY          7       /* High priority for real-time audio */
#define WIFI_TASK_CORE              0
#define WIFI_TX_BUF_SIZE            (16 * 1024)
#define WIFI_RX_BUF_SIZE            (16 * 1024)
#define WIFI_RECONNECT_INTERVAL_MS  3000
#define WIFI_NVS_NAMESPACE          "wifi_creds"
#define WIFI_SCAN_MAX_APS           20

/* ============================
 * Application State
 * ============================ */
typedef enum {
    APP_STATE_INIT = 0,
    APP_STATE_WIFI_SETUP,           /* Showing WiFi setup / Find Devices screen */
    APP_STATE_WIFI_SCANNING,        /* Scanning for WiFi networks */
    APP_STATE_WIFI_CONNECTING,      /* Connecting to hotspot */
    APP_STATE_WIFI_CONNECTED,       /* WiFi connected, TCP connecting */
    APP_STATE_READY,
    APP_STATE_LISTENING,
    APP_STATE_PROCESSING,
    APP_STATE_RESPONDING,
    APP_STATE_ERROR,
} app_state_t;

typedef enum {
    APP_MODE_VOICE = 0,
    APP_MODE_TEXT,
} app_mode_t;

/* ============================
 * FreeRTOS Task Configuration
 * ============================ */
#define APP_TASK_STACK_SIZE     (8 * 1024)
#define APP_TASK_PRIORITY       5
#define APP_TASK_CORE           0

#define AUDIO_TASK_STACK_SIZE   (8 * 1024)
#define AUDIO_TASK_PRIORITY     6
#define AUDIO_TASK_CORE         1

/* ============================
 * Inter-task Event Bits
 * ============================ */
#define EVT_RECORDING_DONE     (1 << 0)
#define EVT_PLAYBACK_DONE      (1 << 1)
#define EVT_WIFI_CONNECTED     (1 << 2)
#define EVT_MODE_CHANGED       (1 << 3)
#define EVT_START_RECORDING    (1 << 4)
#define EVT_STOP_PLAYBACK      (1 << 5)
#define EVT_WIFI_DISCONNECTED  (1 << 6)
#define EVT_START_STREAMING    (1 << 10)
#define EVT_STOP_STREAMING     (1 << 11)

/* ============================
 * Gesture Detection
 * ============================ */
#define SWIPE_THRESHOLD_PX      60
#define SWIPE_MAX_CROSS_PX      100
#define SWIPE_UP_BOTTOM_ZONE_PX 130     /* Swipe-up only triggers from bottom zone */

/* ============================
 * Double-Tap Detection
 * ============================ */
#define DOUBLE_TAP_MAX_TIME_MS      400
#define DOUBLE_TAP_MAX_DIST_PX      50
#define DOUBLE_TAP_DEBOUNCE_US      (50 * 1000)

#endif /* APP_CONFIG_H */
