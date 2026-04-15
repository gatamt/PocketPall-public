#include "touch_hal.h"
#include "app_config.h"
#include "power_manager.h"
#include "esp_log.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "touch";

static gesture_callback_t s_gesture_cb = NULL;
static esp_lcd_touch_handle_t s_tp_handle = NULL;
static lv_indev_drv_t s_indev_drv;
static QueueHandle_t s_gesture_queue = NULL;
static TaskHandle_t s_gesture_task = NULL;
static volatile lv_disp_rot_t s_rotation = LV_DISP_ROT_NONE;

/* Gesture tracking state */
static int16_t s_press_x = 0;
static int16_t s_press_y = 0;
static int16_t s_last_x = 0;
static int16_t s_last_y = 0;
static bool s_pressed = false;

/* Image mode: suppress normal gestures, only emit double-tap */
static volatile bool s_image_mode = false;
static volatile bool s_polling_enabled = true;

/* Double-tap detection state */
static int64_t s_dtap_first_time_us = 0;
static int16_t s_dtap_first_x = 0;
static int16_t s_dtap_first_y = 0;
static bool s_dtap_waiting = false;
static int64_t s_dtap_last_release_us = 0;

/* Touch read robustness */
static lv_indev_state_t s_cached_state = LV_INDEV_STATE_RELEASED;
static uint16_t s_cached_x = 0;
static uint16_t s_cached_y = 0;
static int64_t s_last_poll_us = 0;
static int64_t s_recover_until_us = 0;
static int64_t s_reset_low_since_us = 0;
static int64_t s_last_reset_us = 0;
static uint16_t s_read_fail_streak = 0;

#define TOUCH_POLL_INTERVAL_US         16000
#define TOUCH_RETRY_BACKOFF_BASE_US    20000
#define TOUCH_RETRY_BACKOFF_STEP_US    15000
#define TOUCH_RETRY_BACKOFF_MAX_US     200000
#define TOUCH_RESET_PULSE_US           5000
#define TOUCH_RECOVER_SETTLE_US        120000
#define TOUCH_RESET_MIN_GAP_US         1000000
#define TOUCH_RESET_FAIL_THRESHOLD     10
#define TOUCH_RESET_MIN_GAP_IMAGE_US   250000
#define TOUCH_RESET_FAIL_IMAGE_THRESHOLD 4

/* Keep custom gesture directions aligned with the rendered screen orientation. */
static inline void touch_transform_for_gesture(int16_t *x, int16_t *y)
{
    int16_t tx = *x;
    int16_t ty = *y;

    if (s_rotation == LV_DISP_ROT_180 || s_rotation == LV_DISP_ROT_270) {
        tx = (int16_t)(LCD_H_RES - tx - 1);
        ty = (int16_t)(LCD_V_RES - ty - 1);
    }
    if (s_rotation == LV_DISP_ROT_90 || s_rotation == LV_DISP_ROT_270) {
        int16_t tmp = ty;
        ty = tx;
        tx = (int16_t)(LCD_V_RES - tmp - 1);
    }

    *x = tx;
    *y = ty;
}

static void touch_gesture_dispatch_task(void *arg)
{
    (void)arg;
    gesture_t gesture = GESTURE_NONE;

    while (1) {
        if (xQueueReceive(s_gesture_queue, &gesture, portMAX_DELAY) == pdTRUE) {
            if (s_gesture_cb) {
                s_gesture_cb(gesture);
            }
        }
    }
}

static inline void touch_emit_gesture(gesture_t gesture)
{
    if (gesture == GESTURE_NONE || s_gesture_queue == NULL) {
        return;
    }
    xQueueSend(s_gesture_queue, &gesture, 0);
}

static void touch_gesture_check_image(int16_t release_x, int16_t release_y)
{
    int64_t now_us = esp_timer_get_time();

    /* Debounce: ignore taps within 50ms of each other */
    if (s_dtap_last_release_us != 0 &&
        (now_us - s_dtap_last_release_us) < DOUBLE_TAP_DEBOUNCE_US) {
        return;
    }
    s_dtap_last_release_us = now_us;

    if (s_dtap_waiting) {
        int64_t elapsed_us = now_us - s_dtap_first_time_us;
        if (elapsed_us <= (int64_t)DOUBLE_TAP_MAX_TIME_MS * 1000) {
            int16_t dist = (release_x > s_dtap_first_x ? release_x - s_dtap_first_x
                                                        : s_dtap_first_x - release_x)
                         + (release_y > s_dtap_first_y ? release_y - s_dtap_first_y
                                                        : s_dtap_first_y - release_y);
            if (dist <= DOUBLE_TAP_MAX_DIST_PX) {
                /* Double-tap detected! */
                touch_emit_gesture(GESTURE_DOUBLE_TAP);
                s_dtap_waiting = false;
                return;
            }
        }
        /* Second tap was out of time/range → treat as new first tap */
    }

    /* Record as first tap */
    s_dtap_first_time_us = now_us;
    s_dtap_first_x = release_x;
    s_dtap_first_y = release_y;
    s_dtap_waiting = true;
}

static void touch_gesture_check(int16_t release_x, int16_t release_y)
{
    /* In image mode, only detect double-tap */
    if (s_image_mode) {
        touch_gesture_check_image(release_x, release_y);
        return;
    }

    int16_t dx = release_x - s_press_x;
    int16_t dy = release_y - s_press_y;
    int16_t abs_dx = dx < 0 ? -dx : dx;
    int16_t abs_dy = dy < 0 ? -dy : dy;

    gesture_t gesture = GESTURE_NONE;

    if (abs_dx > SWIPE_THRESHOLD_PX && abs_dy < SWIPE_MAX_CROSS_PX) {
        /* FT3168 X-axis is mirrored relative to screen coordinates on this board */
        gesture = (dx > 0) ? GESTURE_SWIPE_LEFT : GESTURE_SWIPE_RIGHT;
    } else if (abs_dy > SWIPE_THRESHOLD_PX && abs_dx < SWIPE_MAX_CROSS_PX) {
        if (dy < 0) {
            /* Swipe-up: only accept if started near the bottom of the screen */
            int16_t screen_h = (s_rotation == LV_DISP_ROT_90 || s_rotation == LV_DISP_ROT_270)
                               ? LCD_H_RES : LCD_V_RES;
            if (s_press_y >= (screen_h - SWIPE_UP_BOTTOM_ZONE_PX)) {
                gesture = GESTURE_SWIPE_UP;
            }
        } else {
            gesture = GESTURE_SWIPE_DOWN;
        }
    } else if (abs_dx < 15 && abs_dy < 15) {
        gesture = GESTURE_TAP;
    }

    touch_emit_gesture(gesture);
}

/* Single touch read path for both LVGL pointer data and gesture detection */
static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    if (!s_polling_enabled) {
        data->state = LV_INDEV_STATE_RELEASED;
        s_cached_state = LV_INDEV_STATE_RELEASED;
        s_pressed = false;
        return;
    }

    if (!s_tp_handle) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    int64_t now_us = esp_timer_get_time();
    if (now_us < s_recover_until_us) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (s_reset_low_since_us != 0) {
        if ((now_us - s_reset_low_since_us) >= TOUCH_RESET_PULSE_US) {
            if (power_manager_io_set_pin(TOUCH_RESET_EXIO_PIN, true) == ESP_OK) {
                s_reset_low_since_us = 0;
                s_recover_until_us = now_us + TOUCH_RECOVER_SETTLE_US;
                s_read_fail_streak = 0;
                ESP_LOGW(TAG, "Touch reset released, waiting to settle");
            }
        }
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if ((now_us - s_last_poll_us) < TOUCH_POLL_INTERVAL_US) {
        data->state = s_cached_state;
        if (s_cached_state == LV_INDEV_STATE_PRESSED) {
            data->point.x = s_cached_x;
            data->point.y = s_cached_y;
        }
        return;
    }
    s_last_poll_us = now_us;

    uint16_t x = 0, y = 0;
    uint8_t touch_cnt = 0;
    bool pressed = false;
    esp_err_t read_ret = esp_lcd_touch_read_data(s_tp_handle);
    if (read_ret == ESP_OK) {
        pressed = esp_lcd_touch_get_coordinates(s_tp_handle, &x, &y, NULL, &touch_cnt, 1);
        s_read_fail_streak = 0;
    } else {
        s_read_fail_streak++;
        if (s_read_fail_streak >= 2) {
            int64_t backoff_us = TOUCH_RETRY_BACKOFF_BASE_US +
                                 (int64_t)(s_read_fail_streak - 2) * TOUCH_RETRY_BACKOFF_STEP_US;
            if (backoff_us > TOUCH_RETRY_BACKOFF_MAX_US) {
                backoff_us = TOUCH_RETRY_BACKOFF_MAX_US;
            }
            s_recover_until_us = now_us + backoff_us;
        }

        uint16_t reset_fail_threshold = s_image_mode
                                        ? TOUCH_RESET_FAIL_IMAGE_THRESHOLD
                                        : TOUCH_RESET_FAIL_THRESHOLD;
        int64_t reset_min_gap_us = s_image_mode
                                   ? TOUCH_RESET_MIN_GAP_IMAGE_US
                                   : TOUCH_RESET_MIN_GAP_US;

        if (s_read_fail_streak >= reset_fail_threshold &&
            (now_us - s_last_reset_us) >= reset_min_gap_us) {
            if (power_manager_io_set_pin(TOUCH_RESET_EXIO_PIN, false) == ESP_OK) {
                s_last_reset_us = now_us;
                s_reset_low_since_us = now_us;
                s_read_fail_streak = 0;
                ESP_LOGW(TAG, "Touch read repeatedly failed, pulsing reset");
            }
        }
    }

    if (pressed && touch_cnt > 0) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
        s_cached_state = LV_INDEV_STATE_PRESSED;
        s_cached_x = x;
        s_cached_y = y;

        int16_t gx = (int16_t)x;
        int16_t gy = (int16_t)y;
        touch_transform_for_gesture(&gx, &gy);

        if (!s_pressed) {
            s_press_x = gx;
            s_press_y = gy;
            s_pressed = true;
        }
        s_last_x = gx;
        s_last_y = gy;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        s_cached_state = LV_INDEV_STATE_RELEASED;
        if (s_pressed) {
            touch_gesture_check(s_last_x, s_last_y);
            s_pressed = false;
        }
    }
}

esp_err_t touch_hal_init(i2c_master_bus_handle_t bus_handle, lv_disp_t *disp,
                         lv_indev_t **ret_indev)
{
    ESP_LOGI(TAG, "Initializing FT3168 touch controller...");

    /* Create I2C panel IO for touch */
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_config.scl_speed_hz = I2C_MASTER_FREQ_HZ;

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(bus_handle, &tp_io_config, &tp_io_handle));

    /* Configure touch panel */
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = -1,        /* Reset via IO expander */
        .int_gpio_num = TOUCH_INT_PIN,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = LCD_MIRROR_X,
            .mirror_y = LCD_MIRROR_Y,
        },
    };

    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &s_tp_handle));

    /* Register touch input directly with LVGL */
    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.disp = disp;
    s_indev_drv.read_cb = lvgl_touch_read_cb;
    *ret_indev = lv_indev_drv_register(&s_indev_drv);
    if (*ret_indev == NULL) {
        ESP_LOGE(TAG, "Failed to add LVGL touch input");
        return ESP_FAIL;
    }
    /* Increase scroll detection limit for capacitive touch jitter (default 10 is too sensitive) */
    (*ret_indev)->driver->scroll_limit = 20;

    if (!s_gesture_queue) {
        s_gesture_queue = xQueueCreate(8, sizeof(gesture_t));
        if (!s_gesture_queue) {
            ESP_LOGE(TAG, "Failed to create gesture queue");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_gesture_task) {
        BaseType_t ok = xTaskCreatePinnedToCore(touch_gesture_dispatch_task, "touch_gesture",
                                                4096, NULL, 4, &s_gesture_task, APP_TASK_CORE);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "Failed to create gesture task");
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Touch controller initialized");
    return ESP_OK;
}

void touch_hal_register_gesture_cb(gesture_callback_t cb)
{
    s_gesture_cb = cb;
}

void touch_hal_set_rotation(lv_disp_rot_t rotation)
{
    s_rotation = rotation;
}

void touch_hal_set_polling_enabled(bool enabled)
{
    s_polling_enabled = enabled;
    if (!enabled) {
        s_cached_state = LV_INDEV_STATE_RELEASED;
        s_pressed = false;
    }
}

void touch_hal_set_image_mode(bool active)
{
    s_image_mode = active;
    if (active) {
        /* If touch was in backoff, allow immediate reads for image dismiss gestures. */
        s_recover_until_us = 0;
        s_last_poll_us = 0;
    }
    /* Reset double-tap state when entering/leaving image mode */
    s_dtap_waiting = false;
    s_dtap_last_release_us = 0;
}
