#include "app_config.h"
#include "power_manager.h"
#include "display_hal.h"
#include "touch_hal.h"
#include "imu_hal.h"
#include "audio_hal.h"
#include "wifi_transport.h"
#include "image_display.h"
#include "video_display.h"
#include "ui_manager.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "cJSON.h"
#include "math.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "main";

#include "driver/gpio.h"

#define VOLUME_STEP_PERCENT     5
#define VOLUME_BUTTON_POLL_MS   50
#define VOLUME_HOLD_DELAY_MS    1000
#define VOLUME_REPEAT_MS        150
#define VOLUME_BTN_BOOT_GPIO    GPIO_NUM_0
#define VOLUME_BTN_PWR_EXIO     4
#define POWER_OFF_HOLD_MS       4000

/* Global event group */
EventGroupHandle_t g_app_events = NULL;

/* Application state */
static volatile app_state_t s_app_state = APP_STATE_INIT;
static volatile app_mode_t  s_app_mode  = APP_MODE_VOICE;

/* Shared I2C bus handle */
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static TaskHandle_t s_volume_btn_task = NULL;
static TaskHandle_t s_rotation_task = NULL;
static lv_disp_t *s_disp = NULL;
static uint8_t s_volume_percent = 95;
static volatile bool s_mic_enabled = true;
static int64_t s_last_nav_swipe_us = 0;
static int64_t s_mic_resume_holdoff_until_us = 0;
static volatile bool s_ai_turn_complete = true;
static int64_t s_ai_response_start_us = 0;
/* 0xFF means there is no active speech window to trace. */
static volatile uint8_t s_chunks_since_vad_open = 0xFF;

/* UI navigation */
static volatile bool s_settings_screen_active = false;

#define NAV_SWIPE_MIN_INTERVAL_US   (220000)

static bool s_image_rx_error_logged = false;

/* Forward declarations */
static void on_stream_audio_chunk(const int16_t *pcm, size_t bytes);
static void on_command_received(const char *json, size_t len);

/* ---- IMA-ADPCM decoder (matches iOS encoder) ---- */

static int16_t s_adpcm_predicted = 0;
static int     s_adpcm_step_idx  = 0;

static const int8_t s_adpcm_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static const int16_t s_adpcm_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static inline int16_t adpcm_decode_sample(uint8_t code)
{
    int step = s_adpcm_step_table[s_adpcm_step_idx];
    int diffq = step >> 3;
    if (code & 4) diffq += step;
    if (code & 2) diffq += (step >> 1);
    if (code & 1) diffq += (step >> 2);
    if (code & 8) diffq = -diffq;

    int pred = s_adpcm_predicted + diffq;
    if (pred > 32767)  pred = 32767;
    if (pred < -32768) pred = -32768;
    s_adpcm_predicted = (int16_t)pred;

    int idx = s_adpcm_step_idx + s_adpcm_index_table[code & 0x0F];
    if (idx < 0)  idx = 0;
    if (idx > 88) idx = 88;
    s_adpcm_step_idx = idx;

    return s_adpcm_predicted;
}

/* Decode buffer — max 506 ADPCM bytes → 1012 PCM samples */
#define ADPCM_DECODE_MAX_SAMPLES 1024
static int16_t s_adpcm_pcm_buf[ADPCM_DECODE_MAX_SAMPLES];

/* Current applied rotation */
static lv_disp_rot_t s_current_rotation = LV_DISP_ROT_NONE;

static void apply_display_rotation(lv_disp_rot_t rotation)
{
    if (!s_disp || rotation == s_current_rotation) return;
    if (ui_manager_is_rotation_animating()) {
        /* Animation in flight */
    }

    s_current_rotation = rotation;
    ui_manager_animate_rotation(s_disp, rotation, touch_hal_set_rotation);
    ESP_LOGI(TAG, "Display rotation -> %d (animated)", (int)rotation);
}

static uint8_t rotation_to_index(lv_disp_rot_t rotation)
{
    switch (rotation) {
    case LV_DISP_ROT_NONE: return 0;
    case LV_DISP_ROT_90:   return 1;
    case LV_DISP_ROT_180:  return 2;
    case LV_DISP_ROT_270:  return 3;
    default:               return 0;
    }
}

static lv_disp_rot_t index_to_rotation(uint8_t idx)
{
    switch (idx & 0x03U) {
    case 0: return LV_DISP_ROT_NONE;
    case 1: return LV_DISP_ROT_90;
    case 2: return LV_DISP_ROT_180;
    case 3: return LV_DISP_ROT_270;
    default: return LV_DISP_ROT_NONE;
    }
}

static lv_disp_rot_t correct_detected_rotation(lv_disp_rot_t raw_rotation)
{
    uint8_t idx = rotation_to_index(raw_rotation);

#if IMU_ROTATE_COUNTER_UI
    idx = (uint8_t)((4U - idx) & 0x03U);
#endif
    idx = (uint8_t)((idx + (IMU_ROTATE_OFFSET_STEPS & 0x03U)) & 0x03U);

    return index_to_rotation(idx);
}

static bool detect_display_rotation(const imu_sample_t *sample, lv_disp_rot_t *rotation_out)
{
    if (!sample || !rotation_out) return false;

    float abs_x = fabsf(sample->ax_g);
    float abs_y = fabsf(sample->ay_g);
    float abs_z = fabsf(sample->az_g);

    if (abs_z > IMU_ROTATE_MAX_FLAT_Z_G) return false;
    if (abs_x < IMU_ROTATE_MIN_TILT_G && abs_y < IMU_ROTATE_MIN_TILT_G) return false;

    if (abs_x > abs_y) {
        *rotation_out = (sample->ax_g > 0.0f) ? LV_DISP_ROT_90 : LV_DISP_ROT_270;
    } else {
        *rotation_out = (sample->ay_g > 0.0f) ? LV_DISP_ROT_180 : LV_DISP_ROT_NONE;
    }
    return true;
}

static void rotation_task(void *arg)
{
    (void)arg;
    lv_disp_rot_t candidate = LV_DISP_ROT_NONE;
    uint8_t stable_count = 0;

    float filt_ax = 0.0f, filt_ay = 0.0f, filt_az = 1.0f;
    bool ema_init = false;

    while (1) {
        imu_sample_t sample = {0};
        if (imu_hal_read_sample(&sample) == ESP_OK) {
            if (!ema_init) {
                filt_ax = sample.ax_g;
                filt_ay = sample.ay_g;
                filt_az = sample.az_g;
                ema_init = true;
            } else {
                filt_ax += IMU_ROTATE_EMA_ALPHA * (sample.ax_g - filt_ax);
                filt_ay += IMU_ROTATE_EMA_ALPHA * (sample.ay_g - filt_ay);
                filt_az += IMU_ROTATE_EMA_ALPHA * (sample.az_g - filt_az);
            }

            imu_sample_t filtered = {
                .ax_g = filt_ax, .ay_g = filt_ay, .az_g = filt_az,
            };

            lv_disp_rot_t next_rotation = LV_DISP_ROT_NONE;
            if (detect_display_rotation(&filtered, &next_rotation)) {
                next_rotation = correct_detected_rotation(next_rotation);
                if (candidate == next_rotation) {
                    if (stable_count < UINT8_MAX) stable_count++;
                } else {
                    candidate = next_rotation;
                    stable_count = 1;
                }

                if (stable_count >= IMU_ROTATE_STABLE_SAMPLES) {
                    apply_display_rotation(candidate);
                    stable_count = 0;

                    /* Send rotation event to iPhone */
                    uint8_t evt[2];
                    evt[0] = 0x06; /* ROTATION event type */
                    evt[1] = rotation_to_index(candidate);
                    wifi_transport_send_input_event(evt, sizeof(evt));
                }
            } else {
                stable_count = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(IMU_ROTATE_POLL_MS));
    }
}

static void set_mic_enabled(bool enabled)
{
    if (s_mic_enabled == enabled) return;
    s_mic_enabled = enabled;

    if (!enabled) {
        if (audio_hal_is_streaming()) {
            audio_hal_stop_streaming();
        }
        if (s_app_state == APP_STATE_LISTENING || s_app_state == APP_STATE_PROCESSING) {
            s_app_state = APP_STATE_READY;
            ui_manager_set_state(APP_STATE_READY);
        }
    } else {
        if (s_app_state == APP_STATE_READY &&
            wifi_transport_is_connected() &&
            !audio_hal_is_streaming() &&
            !audio_hal_is_stream_playing() &&
            !image_display_is_receiving() &&
            esp_timer_get_time() >= s_mic_resume_holdoff_until_us) {
            esp_err_t ret = audio_hal_start_streaming(on_stream_audio_chunk);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to resume mic stream: %s", esp_err_to_name(ret));
            }
        }
    }

    ESP_LOGI(TAG, "Mic %s", enabled ? "enabled" : "disabled");
}

static void on_gesture(gesture_t gesture)
{
    /* Double-tap: dismiss fullscreen image or video */
    if (gesture == GESTURE_DOUBLE_TAP) {
        if (video_display_is_active()) {
            ESP_LOGI(TAG, "Double-tap: dismissing video");
            touch_hal_set_image_mode(false);
            video_display_stop();
            touch_hal_set_polling_enabled(true);
            /* Notify iPhone to stop video */
            wifi_transport_send_command("{\"cmd\":\"video_stop_request\"}");
            return;
        }
        if (image_display_is_active()) {
            ESP_LOGI(TAG, "Double-tap: dismissing image");
            touch_hal_set_image_mode(false);
            image_display_dismiss();
        }
        return;
    }

    if (gesture == GESTURE_SWIPE_LEFT || gesture == GESTURE_SWIPE_RIGHT) {
        int64_t now_us = esp_timer_get_time();
        if ((now_us - s_last_nav_swipe_us) < NAV_SWIPE_MIN_INTERVAL_US) {
            return;
        }
        s_last_nav_swipe_us = now_us;
    }

    if (s_settings_screen_active) {
        if (gesture == GESTURE_SWIPE_RIGHT) {
            s_settings_screen_active = false;
            ui_manager_show_voice_from_settings();
            set_mic_enabled(true);
            ESP_LOGI(TAG, "Switched to VOICE (from settings)");
        }
        return;
    }

    if (gesture == GESTURE_SWIPE_LEFT && s_app_mode == APP_MODE_VOICE) {
        s_settings_screen_active = true;
        ui_manager_show_settings();
        set_mic_enabled(false);
        ESP_LOGI(TAG, "Switched to SETTINGS");
    } else if (gesture == GESTURE_SWIPE_RIGHT && s_app_mode == APP_MODE_VOICE) {
        s_app_mode = APP_MODE_TEXT;
        ui_manager_set_mode(APP_MODE_TEXT);
        ESP_LOGI(TAG, "Switched to TEXT mode");
    } else if (gesture == GESTURE_SWIPE_LEFT && s_app_mode == APP_MODE_TEXT) {
        s_app_mode = APP_MODE_VOICE;
        ui_manager_set_mode(APP_MODE_VOICE);
        set_mic_enabled(true);
        ESP_LOGI(TAG, "Switched to VOICE mode");
    }

    /* Send gesture event to iPhone */
    uint8_t gesture_id = 0;
    switch (gesture) {
    case GESTURE_SWIPE_LEFT:  gesture_id = 1; break;
    case GESTURE_SWIPE_RIGHT: gesture_id = 2; break;
    case GESTURE_SWIPE_UP:    gesture_id = 3; break;
    case GESTURE_SWIPE_DOWN:  gesture_id = 4; break;
    case GESTURE_TAP:         gesture_id = 5; break;
    case GESTURE_DOUBLE_TAP:  gesture_id = 6; break;
    default: return;
    }

    uint8_t evt[2];
    evt[0] = 0x01; /* GESTURE event type */
    evt[1] = gesture_id;
    wifi_transport_send_input_event(evt, sizeof(evt));
}

static void apply_volume_delta(int delta)
{
    int new_volume = (int)s_volume_percent + delta;
    if (new_volume < 0) new_volume = 0;
    if (new_volume > 100) new_volume = 100;
    if ((uint8_t)new_volume == s_volume_percent) return;

    esp_err_t ret = audio_hal_set_volume((uint8_t)new_volume);
    if (ret == ESP_OK) {
        s_volume_percent = (uint8_t)new_volume;
        ui_manager_show_volume(s_volume_percent);
        ESP_LOGI(TAG, "Volume set to %u%%", s_volume_percent);
    }
}

static void volume_button_task(void *arg)
{
    (void)arg;
    bool prev_boot_pressed = false;
    bool prev_pwr_pressed = false;
    uint32_t boot_hold_ms = 0;
    uint32_t pwr_hold_ms = 0;
    uint32_t boot_repeat_accum = 0;
    uint32_t pwr_repeat_accum = 0;
    uint32_t both_hold_ms = 0;

    gpio_config_t boot_cfg = {
        .pin_bit_mask = (1ULL << VOLUME_BTN_BOOT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot_cfg);

    while (1) {
        bool boot_pressed = (gpio_get_level(VOLUME_BTN_BOOT_GPIO) == 0);

        bool pwr_level = false;
        bool pwr_pressed = false;
        esp_err_t ret = power_manager_io_get_pin(VOLUME_BTN_PWR_EXIO, &pwr_level);
        if (ret == ESP_OK) {
            pwr_pressed = pwr_level;
        }

        /* Both buttons held: power off */
        if (boot_pressed && pwr_pressed) {
            both_hold_ms += VOLUME_BUTTON_POLL_MS;
            if (both_hold_ms >= POWER_OFF_HOLD_MS) {
                ESP_LOGW(TAG, "Both buttons held %lu ms — powering off!", (unsigned long)POWER_OFF_HOLD_MS);
                power_manager_shutdown();
            }
            boot_hold_ms = 0; boot_repeat_accum = 0;
            pwr_hold_ms = 0; pwr_repeat_accum = 0;
            prev_boot_pressed = boot_pressed;
            prev_pwr_pressed = pwr_pressed;
            vTaskDelay(pdMS_TO_TICKS(VOLUME_BUTTON_POLL_MS));
            continue;
        } else {
            both_hold_ms = 0;
        }

        /* BOOT = Volume UP */
        if (boot_pressed && !pwr_pressed) {
            if (!prev_boot_pressed) {
                apply_volume_delta(+VOLUME_STEP_PERCENT);
                boot_hold_ms = 0; boot_repeat_accum = 0;
            } else {
                boot_hold_ms += VOLUME_BUTTON_POLL_MS;
                if (boot_hold_ms >= VOLUME_HOLD_DELAY_MS) {
                    boot_repeat_accum += VOLUME_BUTTON_POLL_MS;
                    if (boot_repeat_accum >= VOLUME_REPEAT_MS) {
                        boot_repeat_accum = 0;
                        apply_volume_delta(+VOLUME_STEP_PERCENT);
                    }
                }
            }
        } else {
            boot_hold_ms = 0; boot_repeat_accum = 0;
        }

        /* PWR = Volume DOWN */
        if (pwr_pressed && !boot_pressed) {
            if (!prev_pwr_pressed) {
                apply_volume_delta(-VOLUME_STEP_PERCENT);
                pwr_hold_ms = 0; pwr_repeat_accum = 0;
            } else {
                pwr_hold_ms += VOLUME_BUTTON_POLL_MS;
                if (pwr_hold_ms >= VOLUME_HOLD_DELAY_MS) {
                    pwr_repeat_accum += VOLUME_BUTTON_POLL_MS;
                    if (pwr_repeat_accum >= VOLUME_REPEAT_MS) {
                        pwr_repeat_accum = 0;
                        apply_volume_delta(-VOLUME_STEP_PERCENT);
                    }
                }
            }
        } else {
            pwr_hold_ms = 0; pwr_repeat_accum = 0;
        }

        /* Send button events to iPhone */
        if (boot_pressed != prev_boot_pressed) {
            uint8_t evt[3];
            evt[0] = 0x05; /* BUTTON event type */
            evt[1] = 0x00; /* button_id: BOOT */
            evt[2] = boot_pressed ? 0x01 : 0x00;
            wifi_transport_send_input_event(evt, sizeof(evt));
        }
        if (pwr_pressed != prev_pwr_pressed) {
            uint8_t evt[3];
            evt[0] = 0x05; /* BUTTON event type */
            evt[1] = 0x01; /* button_id: PWR */
            evt[2] = pwr_pressed ? 0x01 : 0x00;
            wifi_transport_send_input_event(evt, sizeof(evt));
        }

        prev_boot_pressed = boot_pressed;
        prev_pwr_pressed = pwr_pressed;
        vTaskDelay(pdMS_TO_TICKS(VOLUME_BUTTON_POLL_MS));
    }
}

/* ---- Audio callbacks ---- */

/* Streaming callback: sends audio chunks to iPhone via WiFi TCP */
static void on_stream_audio_chunk(const int16_t *pcm, size_t bytes)
{
    static uint32_t s_mic_audio_drop_count = 0;
    static uint32_t s_mic_audio_first_chunk_drop_count = 0;
    static uint32_t s_mic_audio_chunk_total = 0;
    static uint32_t s_mic_audio_send_ok_count = 0;

    if (!s_mic_enabled || !wifi_transport_is_connected()) {
        return;
    }

    s_mic_audio_chunk_total++;

    uint8_t trace_idx = s_chunks_since_vad_open;
    bool trace_chunk = (trace_idx != 0xFFU && trace_idx < 3U);

    esp_err_t ret = ESP_FAIL;
    int attempts_used = 0;
    for (int attempt = 0; attempt <= MIC_AUDIO_SEND_RETRY_COUNT; attempt++) {
        attempts_used = attempt + 1;
        ret = wifi_transport_send_audio(pcm, bytes);
        if (ret == ESP_OK) {
            s_mic_audio_send_ok_count++;
            if (trace_chunk) {
                ESP_LOGI(TAG, "Mic chunk after VAD open #%u sent (attempt=%d, bytes=%u)",
                         (unsigned)(trace_idx + 1U), attempts_used, (unsigned)bytes);
            }
            if (s_chunks_since_vad_open != 0xFFU) {
                s_chunks_since_vad_open++;
            }
            if ((s_mic_audio_chunk_total % 100U) == 0U) {
                float drop_rate = (s_mic_audio_chunk_total > 0U)
                                      ? (100.0f * (float)s_mic_audio_drop_count / (float)s_mic_audio_chunk_total)
                                      : 0.0f;
                ESP_LOGI(TAG,
                         "Mic TX stats: total=%lu ok=%lu drop=%lu first_drop=%lu drop_rate=%.1f%%",
                         (unsigned long)s_mic_audio_chunk_total,
                         (unsigned long)s_mic_audio_send_ok_count,
                         (unsigned long)s_mic_audio_drop_count,
                         (unsigned long)s_mic_audio_first_chunk_drop_count,
                         drop_rate);
            }
            return;
        }
        if (attempt < MIC_AUDIO_SEND_RETRY_COUNT) {
            const TickType_t retry_delay =
                (attempt == 0) ? pdMS_TO_TICKS(1) : pdMS_TO_TICKS(4);
            vTaskDelay(retry_delay);
            if (!s_mic_enabled || !wifi_transport_is_connected()) {
                break;
            }
        }
    }

    s_mic_audio_drop_count++;
    if (trace_idx == 0U) {
        s_mic_audio_first_chunk_drop_count++;
    }
    if (trace_chunk) {
        ESP_LOGW(TAG, "Mic chunk after VAD open #%u dropped (attempts=%d, err=%s)",
                 (unsigned)(trace_idx + 1U), attempts_used, esp_err_to_name(ret));
    }
    if (s_chunks_since_vad_open != 0xFFU) {
        s_chunks_since_vad_open++;
    }

    if ((s_mic_audio_drop_count % 20U) == 1U) {
        ESP_LOGW(TAG,
                 "Dropping mic chunk #%lu (send failed: %s, first_chunk_drops=%lu)",
                 (unsigned long)s_mic_audio_drop_count, esp_err_to_name(ret),
                 (unsigned long)s_mic_audio_first_chunk_drop_count);
    }
    if ((s_mic_audio_chunk_total % 100U) == 0U) {
        float drop_rate = (s_mic_audio_chunk_total > 0U)
                              ? (100.0f * (float)s_mic_audio_drop_count / (float)s_mic_audio_chunk_total)
                              : 0.0f;
        ESP_LOGI(TAG,
                 "Mic TX stats: total=%lu ok=%lu drop=%lu first_drop=%lu drop_rate=%.1f%%",
                 (unsigned long)s_mic_audio_chunk_total,
                 (unsigned long)s_mic_audio_send_ok_count,
                 (unsigned long)s_mic_audio_drop_count,
                 (unsigned long)s_mic_audio_first_chunk_drop_count,
                 drop_rate);
    }
}

/* VAD callback: UI feedback */
static void on_vad(bool speech_active)
{
    static bool s_was_speaking = false;

    if (!s_mic_enabled) {
        s_was_speaking = false;
        s_chunks_since_vad_open = 0xFF;
        return;
    }

    if (speech_active) {
        if (!s_was_speaking) {
            s_chunks_since_vad_open = 0;
            s_app_state = APP_STATE_LISTENING;
            ui_manager_set_state(APP_STATE_LISTENING);
            ui_manager_clear_response_text();

            /* Barge-in: if playing, flush playback */
            if (audio_hal_is_stream_playing()) {
                ESP_LOGI(TAG, "Barge-in detected, flushing playback");
                audio_hal_stream_flush();
            }

            /* Notify iPhone that user started speaking */
            wifi_transport_send_command("{\"cmd\":\"user_state\",\"state\":\"listening\"}");
        }
        s_was_speaking = true;
    } else if (s_was_speaking) {
        if (s_app_state == APP_STATE_LISTENING) {
            s_app_state = APP_STATE_PROCESSING;
            ui_manager_set_state(APP_STATE_PROCESSING);
        }
        /* Notify iPhone that user stopped speaking */
        wifi_transport_send_command("{\"cmd\":\"user_state\",\"state\":\"processing\"}");
        s_was_speaking = false;
        s_chunks_since_vad_open = 0xFF;
    }
}

/* Audio received from iPhone (AI response) — IMA-ADPCM encoded */
static void on_wifi_audio(const uint8_t *data, size_t bytes)
{
    if (!audio_hal_is_stream_playing()) {
        /* Reset ADPCM decoder state for new response */
        s_adpcm_predicted = 0;
        s_adpcm_step_idx  = 0;
        s_ai_turn_complete = false;
        s_ai_response_start_us = esp_timer_get_time();

        s_app_state = APP_STATE_RESPONDING;
        ui_manager_set_state(APP_STATE_RESPONDING);
        if (!(image_display_is_active() && !image_display_is_receiving())) {
            touch_hal_set_polling_enabled(false);
        }

        /* Stop mic while playing */
        if (audio_hal_is_streaming()) {
            audio_hal_stop_streaming();
        }

        if (audio_hal_start_stream_playback(AUDIO_SAMPLE_RATE_PLAY) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start stream playback");
            touch_hal_set_polling_enabled(true);
            s_app_state = APP_STATE_READY;
            ui_manager_set_state(APP_STATE_READY);
            return;
        }
    }

    /* Decode IMA-ADPCM to PCM: each input byte → 2 output samples */
    const uint8_t *adpcm = data;
    size_t adpcm_len = bytes;
    size_t pcm_samples = adpcm_len * 2;
    if (pcm_samples > ADPCM_DECODE_MAX_SAMPLES) {
        pcm_samples = ADPCM_DECODE_MAX_SAMPLES;
        adpcm_len = pcm_samples / 2;
    }

    for (size_t i = 0; i < adpcm_len; i++) {
        uint8_t byte = adpcm[i];
        s_adpcm_pcm_buf[i * 2]     = adpcm_decode_sample(byte & 0x0F);
        s_adpcm_pcm_buf[i * 2 + 1] = adpcm_decode_sample((byte >> 4) & 0x0F);
    }

    if (audio_hal_stream_write(s_adpcm_pcm_buf, pcm_samples * sizeof(int16_t)) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to queue WiFi audio chunk");
    }
}

/* Command received from iPhone (JSON over WiFi TCP) */
static void on_command_received(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse command JSON");
        return;
    }

    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cmd || !cmd->valuestring) {
        cJSON_Delete(root);
        return;
    }

    const char *cmd_str = cmd->valuestring;

    if (strcmp(cmd_str, "ai_state") == 0) {
        cJSON *state = cJSON_GetObjectItem(root, "state");
        if (state && state->valuestring) {
            if (strcmp(state->valuestring, "responding") == 0) {
                s_ai_turn_complete = false;
                if (s_ai_response_start_us == 0) {
                    s_ai_response_start_us = esp_timer_get_time();
                }
                if (audio_hal_is_streaming()) {
                    audio_hal_stop_streaming();
                }
                if (!(image_display_is_active() && !image_display_is_receiving())) {
                    touch_hal_set_polling_enabled(false);
                }
                s_app_state = APP_STATE_RESPONDING;
                ui_manager_set_state(APP_STATE_RESPONDING);
            } else if (strcmp(state->valuestring, "processing") == 0) {
                s_app_state = APP_STATE_PROCESSING;
                ui_manager_set_state(APP_STATE_PROCESSING);
            } else if (strcmp(state->valuestring, "ready") == 0 ||
                       strcmp(state->valuestring, "listening") == 0) {
                s_ai_turn_complete = true;
                audio_hal_stream_end();
                if (!audio_hal_is_stream_playing() && !image_display_is_receiving()) {
                    touch_hal_set_polling_enabled(true);
                }
            } else if (strcmp(state->valuestring, "error") == 0) {
                s_ai_turn_complete = true;
                audio_hal_stream_flush();
                if (!audio_hal_is_stream_playing() && !image_display_is_receiving()) {
                    touch_hal_set_polling_enabled(true);
                }
            }
        }
    } else if (strcmp(cmd_str, "transcript") == 0) {
        cJSON *text = cJSON_GetObjectItem(root, "text");
        if (text && text->valuestring) {
            ui_manager_set_response_text(text->valuestring);
        }
    } else if (strcmp(cmd_str, "device_info") == 0) {
        cJSON *name = cJSON_GetObjectItem(root, "name");
        if (name && name->valuestring) {
            ui_manager_set_wifi_status(true);
        }
    } else if (strcmp(cmd_str, "video_start") == 0) {
        ESP_LOGI(TAG, "Video start command received");
        /* Dismiss active image if any */
        if (image_display_is_active()) {
            touch_hal_set_image_mode(false);
            image_display_dismiss();
        }
        /* Stop mic during video */
        if (audio_hal_is_streaming()) {
            audio_hal_stop_streaming();
        }
        esp_err_t ret = video_display_start();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "video_start failed: %s", esp_err_to_name(ret));
            ui_manager_show_error("Video init failed");
        } else {
            touch_hal_set_image_mode(true);      /* Enable double-tap detection */
            touch_hal_set_polling_enabled(true);  /* Keep touch for double-tap dismiss */
        }
    } else if (strcmp(cmd_str, "video_stop") == 0) {
        ESP_LOGI(TAG, "Video stop command received");
        if (video_display_is_active()) {
            touch_hal_set_image_mode(false);
            video_display_stop();
            touch_hal_set_polling_enabled(true);
        }
    } else if (strcmp(cmd_str, "image_start") == 0) {
        cJSON *w = cJSON_GetObjectItem(root, "width");
        cJSON *h = cJSON_GetObjectItem(root, "height");
        cJSON *sz = cJSON_GetObjectItem(root, "size");
        if (w && h && sz) {
            if (image_display_is_active()) {
                ESP_LOGI(TAG, "image_start while image active: dismissing previous image");
                touch_hal_set_image_mode(false);
                image_display_dismiss();
            }
            s_image_rx_error_logged = false;
            esp_err_t ret = image_display_begin((uint16_t)w->valueint,
                                                (uint16_t)h->valueint,
                                                (size_t)sz->valueint);
            if (ret == ESP_OK) {
                if (audio_hal_is_streaming()) {
                    audio_hal_stop_streaming();
                }
                s_mic_resume_holdoff_until_us =
                    esp_timer_get_time() + ((int64_t)MIC_RESUME_HOLDOFF_AFTER_IMAGE_MS * 1000);
                touch_hal_set_image_mode(true);
                touch_hal_set_polling_enabled(false);
            } else {
                touch_hal_set_image_mode(false);
                touch_hal_set_polling_enabled(true);
                ui_manager_show_error("Image format not supported");
                ESP_LOGW(TAG, "image_start rejected (%s)", esp_err_to_name(ret));
            }
        }
    } else if (strcmp(cmd_str, "image_done") == 0) {
        esp_err_t ret = image_display_finish();
        if (ret == ESP_OK) {
            touch_hal_set_polling_enabled(true);
        } else if (ret == ESP_ERR_NOT_FINISHED) {
            ESP_LOGW(TAG, "image_done received before payload complete");
        } else {
            touch_hal_set_image_mode(false);
            image_display_dismiss();
            touch_hal_set_polling_enabled(true);
            ui_manager_show_error("Image transfer failed");
            ESP_LOGW(TAG, "image_done failed (%s)", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(TAG, "Unknown command: %s", cmd_str);
    }

    cJSON_Delete(root);
}

/* Image chunk received from iPhone */
static void on_wifi_image(const uint8_t *data, size_t len)
{
    esp_err_t ret = image_display_write_chunk(data, len);
    if (ret == ESP_OK) {
        if (image_display_is_active() && !image_display_is_receiving()) {
            touch_hal_set_polling_enabled(true);
        }
        return;
    }
    if (ret != ESP_OK && !s_image_rx_error_logged) {
        ESP_LOGW(TAG, "Image chunk rejected (%s)", esp_err_to_name(ret));
        s_image_rx_error_logged = true;
    }
}

/* Video JPEG data received from iPhone */
static void on_wifi_video(const uint8_t *data, size_t len, uint8_t flags)
{
    if (!video_display_is_active()) return;  /* Drop frames after stop */
    video_display_feed(data, len, flags);
}

/* Display command received from iPhone */
static void on_display_cmd(const uint8_t *data, size_t len)
{
    if (len < 1) return;
    uint8_t cmd_id = data[0];
    const uint8_t *params = data + 1;
    size_t params_len = len - 1;

    switch (cmd_id) {
    case 0x01: /* SET_STATE */
        if (params_len >= 1) {
            app_state_t new_state = (app_state_t)params[0];
            s_app_state = new_state;
            ui_manager_set_state(new_state);
        }
        break;
    case 0x02: /* SET_TEXT */
        if (params_len >= 2) {
            uint16_t text_len = (uint16_t)(params[0] | (params[1] << 8));
            if (params_len >= 2 + text_len) {
                char text_buf[512];
                size_t copy_len = text_len < sizeof(text_buf) - 1 ? text_len : sizeof(text_buf) - 1;
                memcpy(text_buf, params + 2, copy_len);
                text_buf[copy_len] = '\0';
                ui_manager_set_response_text(text_buf);
            }
        }
        break;
    case 0x03: /* CLEAR_TEXT */
        ui_manager_clear_response_text();
        break;
    case 0x04: /* SET_SOUNDBAR */
        if (params_len >= 2) {
            uint16_t energy = (uint16_t)(params[0] | (params[1] << 8));
            ui_manager_update_soundbar(energy);
        }
        break;
    case 0x05: /* SHOW_ERROR */
        if (params_len >= 2) {
            uint16_t text_len = (uint16_t)(params[0] | (params[1] << 8));
            if (params_len >= 2 + text_len) {
                char err_buf[256];
                size_t copy_len = text_len < sizeof(err_buf) - 1 ? text_len : sizeof(err_buf) - 1;
                memcpy(err_buf, params + 2, copy_len);
                err_buf[copy_len] = '\0';
                ui_manager_show_error(err_buf);
            }
        }
        break;
    case 0x06: /* SET_BATTERY */
        if (params_len >= 3) {
            ui_manager_set_battery_level(params[0]);
            ui_manager_set_charging_status(params[1], params[2]);
        }
        break;
    case 0x07: /* SHOW_VOLUME */
        if (params_len >= 1) {
            ui_manager_show_volume(params[0]);
        }
        break;
    case 0x08: /* IMAGE_START — forwarded via CMD_JSON for consistency */
    case 0x09: /* IMAGE_DONE */
        break;
    default:
        ESP_LOGW(TAG, "Unknown display cmd: 0x%02X", cmd_id);
        break;
    }
}

/* WiFi TCP connection state changed */
static void on_wifi_connection(bool connected)
{
    if (connected) {
        ESP_LOGI(TAG, "WiFi: iPhone connected!");
        s_ai_turn_complete = true;
        s_ai_response_start_us = 0;
        s_app_state = APP_STATE_READY;
        ui_manager_set_state(APP_STATE_READY);
        ui_manager_set_wifi_status(true);
        touch_hal_set_polling_enabled(true);
        xEventGroupSetBits(g_app_events, EVT_WIFI_CONNECTED);

        /* Start streaming mic audio */
        if (s_mic_enabled &&
            !audio_hal_is_streaming() &&
            !audio_hal_is_stream_playing() &&
            !image_display_is_receiving() &&
            esp_timer_get_time() >= s_mic_resume_holdoff_until_us) {
            audio_hal_start_streaming(on_stream_audio_chunk);
        }

        /* Send battery status to iPhone */
        uint8_t batt_pct = 0;
        power_manager_get_battery_percent(&batt_pct);
        bool vbus = power_manager_is_vbus_present();
        bool charging = power_manager_is_charging();
        uint8_t evt[4];
        evt[0] = 0x07; /* BATTERY event type */
        evt[1] = batt_pct;
        evt[2] = vbus ? 1 : 0;
        evt[3] = charging ? 1 : 0;
        wifi_transport_send_input_event(evt, sizeof(evt));
    } else {
        ESP_LOGW(TAG, "WiFi: iPhone disconnected!");
        s_ai_turn_complete = true;
        s_ai_response_start_us = 0;
        xEventGroupSetBits(g_app_events, EVT_WIFI_DISCONNECTED);

        s_app_state = APP_STATE_WIFI_SETUP;
        ui_manager_set_state(APP_STATE_WIFI_SETUP);
        touch_hal_set_polling_enabled(true);

        if (audio_hal_is_streaming()) {
            audio_hal_stop_streaming();
        }
        if (audio_hal_is_stream_playing()) {
            audio_hal_stream_flush();
        }

        if (video_display_is_active()) {
            touch_hal_set_image_mode(false);
            video_display_stop();
        }

        if (image_display_is_active() && image_display_is_receiving()) {
            touch_hal_set_image_mode(false);
            image_display_dismiss();
        }

        ui_manager_set_wifi_status(false);
    }
}

/* ---- Main application task ---- */

static void app_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "App task started on core %d", xPortGetCoreID());

    /* Register VAD callback for UI */
    audio_hal_register_vad_cb(on_vad);

    uint32_t battery_update_counter = 0;

    while (1) {
        /*
         * Keep double-tap dismiss available while an image or video is shown,
         * even if other app states temporarily toggled touch polling.
         */
        if ((image_display_is_active() && !image_display_is_receiving()) ||
            video_display_is_active()) {
            touch_hal_set_polling_enabled(true);
        }

        /* Periodically update battery and charging status */
        battery_update_counter++;
        if (battery_update_counter >= 300) {
            battery_update_counter = 0;
            uint8_t batt_pct = 0;
            if (power_manager_get_battery_percent(&batt_pct) == ESP_OK) {
                ui_manager_set_battery_level(batt_pct);
            }
            bool vbus = power_manager_is_vbus_present();
            bool charging = power_manager_is_charging();
            ui_manager_set_charging_status(vbus, charging);

            /* Also send battery to iPhone if connected */
            if (wifi_transport_is_connected()) {
                uint8_t evt[4];
                evt[0] = 0x07; /* BATTERY event type */
                evt[1] = batt_pct;
                evt[2] = vbus ? 1 : 0;
                evt[3] = charging ? 1 : 0;
                wifi_transport_send_input_event(evt, sizeof(evt));
            }
        }

        switch (s_app_state) {
        case APP_STATE_WIFI_SETUP:
            vTaskDelay(pdMS_TO_TICKS(100));
            break;

        case APP_STATE_READY:
        case APP_STATE_LISTENING:
        case APP_STATE_PROCESSING: {
            if (!s_mic_enabled && audio_hal_is_streaming()) {
                audio_hal_stop_streaming();
            }

            if (s_mic_enabled &&
                wifi_transport_is_connected() &&
                !audio_hal_is_streaming() &&
                !audio_hal_is_stream_playing() &&
                !image_display_is_receiving() &&
                esp_timer_get_time() >= s_mic_resume_holdoff_until_us) {
                audio_hal_start_streaming(on_stream_audio_chunk);
            }

            /* Update sound bars from energy level */
            uint16_t energy = audio_hal_get_current_energy();
            if (s_app_state == APP_STATE_LISTENING) {
                ui_manager_update_soundbar(energy);
            }

            /* If WiFi disconnected, return to setup */
            if (!wifi_transport_is_connected() &&
                s_app_state != APP_STATE_WIFI_SETUP) {
                s_app_state = APP_STATE_WIFI_SETUP;
                ui_manager_set_state(APP_STATE_WIFI_SETUP);
            }

            vTaskDelay(pdMS_TO_TICKS(33));
            break;
        }

        case APP_STATE_RESPONDING: {
            bool stream_playing = audio_hal_is_stream_playing();
            bool response_done = false;

            if (s_app_mode == APP_MODE_VOICE) {
                if (stream_playing) {
                    uint16_t energy = audio_hal_get_current_energy();
                    ui_manager_update_soundbar(energy);
                    vTaskDelay(pdMS_TO_TICKS(33));
                    break;
                }
            }

            if (!stream_playing) {
                if (s_ai_turn_complete) {
                    response_done = true;
                } else if (s_ai_response_start_us > 0) {
                    int64_t elapsed_us = esp_timer_get_time() - s_ai_response_start_us;
                    int64_t fallback_us = (int64_t)AI_RESPONSE_FALLBACK_DONE_MS * 1000;
                    if (elapsed_us >= fallback_us) {
                        ESP_LOGW(TAG, "Response fallback hit (no ai_state end signal), finishing turn");
                        response_done = true;
                    }
                }
            }

            if (response_done) {
                ui_manager_update_soundbar(0);
                s_app_state = APP_STATE_READY;
                ui_manager_set_state(APP_STATE_READY);
                if (!image_display_is_receiving()) {
                    touch_hal_set_polling_enabled(true);
                }
                s_ai_turn_complete = true;
                s_ai_response_start_us = 0;
                s_mic_resume_holdoff_until_us = 0;

                if (s_mic_enabled &&
                    wifi_transport_is_connected() &&
                    !audio_hal_is_streaming() &&
                    !audio_hal_is_stream_playing() &&
                    !image_display_is_receiving() &&
                    esp_timer_get_time() >= s_mic_resume_holdoff_until_us) {
                    esp_err_t ret = audio_hal_start_streaming(on_stream_audio_chunk);
                    if (ret != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to restart streaming after response: %s",
                                 esp_err_to_name(ret));
                    }
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            break;
        }

        default:
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }
    }
}

/* ---- WiFi UI callbacks ---- */

static void on_wifi_scan_done(const wifi_scan_result_t *results, uint16_t count)
{
    ESP_LOGI(TAG, "WiFi scan done: %u networks found", count);

    uint16_t ui_count = count < 20 ? count : 20;
    /* Heap-allocate to avoid stack overflow in sys_evt task */
    ui_wifi_network_t *ui_networks = calloc(ui_count, sizeof(ui_wifi_network_t));
    if (!ui_networks) {
        ESP_LOGE(TAG, "Failed to allocate WiFi network list");
        return;
    }

    for (uint16_t i = 0; i < ui_count; i++) {
        strncpy(ui_networks[i].ssid, results[i].ssid, sizeof(ui_networks[i].ssid) - 1);
        ui_networks[i].ssid[sizeof(ui_networks[i].ssid) - 1] = '\0';
        ui_networks[i].rssi = results[i].rssi;
        ui_networks[i].needs_password = (results[i].authmode != 0);
    }

    ui_manager_show_wifi_networks(ui_networks, ui_count);
    free(ui_networks);
}

static void on_wifi_find_pressed(void)
{
    ESP_LOGI(TAG, "WiFi find pressed — starting scan");
    s_app_state = APP_STATE_WIFI_SCANNING;
    ui_manager_show_wifi_scanning();
    wifi_transport_start_scan(on_wifi_scan_done);
}

static void on_wifi_network_selected(const char *ssid, bool needs_password)
{
    ESP_LOGI(TAG, "WiFi network selected: '%s' (password=%d)", ssid, needs_password);

    if (needs_password) {
        ui_manager_show_wifi_password_entry(ssid);
    } else {
        /* Open network — connect directly */
        s_app_state = APP_STATE_WIFI_CONNECTING;
        ui_manager_show_wifi_connecting(ssid);
        wifi_transport_connect_wifi(ssid, "");
        wifi_transport_save_credentials(ssid, "");
    }
}

static void on_wifi_password_submitted(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "WiFi password submitted for '%s'", ssid);
    s_app_state = APP_STATE_WIFI_CONNECTING;
    ui_manager_show_wifi_connecting(ssid);
    wifi_transport_connect_wifi(ssid, password);
    wifi_transport_save_credentials(ssid, password);
}

/* ---- Entry point ---- */

void app_main(void)
{
    /* Keep realtime paths quiet */
    esp_log_level_set("i2s_common", ESP_LOG_NONE);
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_NONE);
    esp_log_level_set("FT5x06", ESP_LOG_NONE);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Pocket AI v2.0 — WiFi Mode");
    ESP_LOGI(TAG, "========================================");

    /* 1. Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Create global event group */
    g_app_events = xEventGroupCreate();
    assert(g_app_events);

    /* 3. Initialize LVGL port */
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_affinity = APP_TASK_CORE;
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));
    ESP_LOGI(TAG, "[1/7] LVGL port initialized");

    /* 4. Initialize shared I2C bus */
    ESP_ERROR_CHECK(power_manager_init_i2c(&s_i2c_bus));
    ESP_LOGI(TAG, "[2/7] I2C bus initialized");

    /* 5. Initialize PMIC and IO expander */
    ret = power_manager_init_pmic(s_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PMIC init failed (%s), continuing", esp_err_to_name(ret));
    }
    ret = power_manager_init_io_expander(s_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "IO expander init failed (%s), continuing", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "[3/7] Power management setup complete");

    /* Charging screen on cold power-on with USB */
    uint8_t boot_reason = power_manager_get_boot_reason();
    esp_reset_reason_t reset_reason = esp_reset_reason();
    bool vbus_present = power_manager_is_vbus_present();
    bool poweron_reset = (reset_reason == ESP_RST_POWERON);

    ESP_LOGI(TAG, "Boot reason register: 0x%02X, Reset reason: %d", boot_reason, (int)reset_reason);

    if (vbus_present && poweron_reset) {
        ESP_LOGI(TAG, "USB present on power-on — entering charging screen mode");

        ESP_ERROR_CHECK(display_hal_init(&s_disp));
        ESP_ERROR_CHECK(ui_manager_create_charging_screen());

        uint8_t chg_pct = 0;
        bool chg_active = power_manager_is_charging();
        power_manager_get_battery_percent(&chg_pct);
        ui_manager_update_charging_screen(chg_pct, chg_active);

        gpio_config_t boot_btn_cfg = {
            .pin_bit_mask = (1ULL << GPIO_NUM_0),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&boot_btn_cfg);

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(500));
            chg_active = power_manager_is_charging();
            if (power_manager_get_battery_percent(&chg_pct) == ESP_OK) {
                ui_manager_update_charging_screen(chg_pct, chg_active);
            }
            if (!power_manager_is_vbus_present()) {
                ESP_LOGW(TAG, "VBUS removed in charging mode — shutting down");
                power_manager_shutdown();
            }
            bool boot_btn = (gpio_get_level(GPIO_NUM_0) == 0);
            bool pwr_btn = false;
            power_manager_io_get_pin(4, &pwr_btn);
            if (boot_btn || pwr_btn) {
                ESP_LOGI(TAG, "Button pressed in charging mode — restarting for normal boot");
                esp_restart();
            }
        }
    }

    /* 6. Initialize display */
    ESP_ERROR_CHECK(display_hal_init(&s_disp));
    ESP_LOGI(TAG, "[4/7] Display initialized");

    /* 7. Initialize touch */
    lv_indev_t *indev = NULL;
    ESP_ERROR_CHECK(touch_hal_init(s_i2c_bus, s_disp, &indev));
    touch_hal_register_gesture_cb(on_gesture);
    touch_hal_set_rotation(LV_DISP_ROT_NONE);
    ESP_LOGI(TAG, "[5/7] Touch initialized");

    /* 8. Initialize WiFi transport (TCP over iPhone Personal Hotspot) */
    wifi_transport_callbacks_t wifi_cbs = {
        .on_audio = on_wifi_audio,
        .on_command = on_command_received,
        .on_image = on_wifi_image,
        .on_display_cmd = on_display_cmd,
        .on_video = on_wifi_video,
        .on_connection = on_wifi_connection,
    };
    ESP_ERROR_CHECK(wifi_transport_init(&wifi_cbs));
    ESP_LOGI(TAG, "[6/7] WiFi transport initialized");

    /* 9. Initialize UI */
    ESP_ERROR_CHECK(ui_manager_init());
    s_app_state = APP_STATE_WIFI_SETUP;
    ui_manager_set_state(APP_STATE_WIFI_SETUP);
    ui_manager_set_wifi_status(false);
    ESP_LOGI(TAG, "[7/7] UI initialized — WiFi setup screen shown");

    /* Register WiFi UI callbacks */
    ui_manager_register_wifi_find_cb(on_wifi_find_pressed);
    ui_manager_register_wifi_network_select_cb(on_wifi_network_selected);
    ui_manager_register_wifi_password_submit_cb(on_wifi_password_submitted);

    /* Auto-connect if WiFi credentials are saved */
    {
        char saved_ssid[33] = {0};
        char saved_pass[65] = {0};
        if (wifi_transport_has_saved_credentials() &&
            wifi_transport_load_credentials(saved_ssid, sizeof(saved_ssid),
                                             saved_pass, sizeof(saved_pass)) == ESP_OK) {
            ESP_LOGI(TAG, "Auto-connecting to saved WiFi: '%s'", saved_ssid);
            s_app_state = APP_STATE_WIFI_CONNECTING;
            ui_manager_show_wifi_connecting(saved_ssid);
            wifi_transport_connect_wifi(saved_ssid, saved_pass);
        }
    }

    /* Initialize image display module */
    ESP_ERROR_CHECK(image_display_init());

    /* Initialize video display module */
    ESP_ERROR_CHECK(video_display_init());

    /* Initialize IMU for auto-rotation */
    ret = imu_hal_init(s_i2c_bus);
    if (ret == ESP_OK) {
        xTaskCreatePinnedToCore(
            rotation_task, "rotation_task",
            4 * 1024, NULL,
            APP_TASK_PRIORITY, &s_rotation_task,
            APP_TASK_CORE
        );
        ESP_LOGI(TAG, "Auto-rotation enabled (QMI8658)");
    } else {
        ESP_LOGW(TAG, "IMU init failed (%s), auto-rotation disabled", esp_err_to_name(ret));
    }

    /* 10. Initialize audio subsystem */
    ESP_ERROR_CHECK(audio_hal_init(s_i2c_bus));
    ESP_ERROR_CHECK(audio_hal_set_volume(s_volume_percent));
    ESP_LOGI(TAG, "Audio initialized");

    /* Start volume button polling task */
    xTaskCreatePinnedToCore(
        volume_button_task, "volume_btn_task",
        3 * 1024, NULL,
        APP_TASK_PRIORITY, &s_volume_btn_task,
        APP_TASK_CORE
    );

    /* Set initial battery level */
    uint8_t batt_pct = 0;
    if (power_manager_get_battery_percent(&batt_pct) == ESP_OK) {
        ui_manager_set_battery_level(batt_pct);
    }
    ui_manager_set_charging_status(
        power_manager_is_vbus_present(),
        power_manager_is_charging()
    );

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Pocket AI v2.0 — WiFi Mode");
    ESP_LOGI(TAG, "  Waiting for WiFi connection...");
    ESP_LOGI(TAG, "========================================");

    /* Create main application task on Core 0 */
    xTaskCreatePinnedToCore(
        app_task, "app_task",
        APP_TASK_STACK_SIZE, NULL,
        APP_TASK_PRIORITY, NULL,
        APP_TASK_CORE
    );
}
