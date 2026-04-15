#ifndef TOUCH_HAL_H
#define TOUCH_HAL_H

#include "esp_err.h"
#include "lvgl.h"
#include "driver/i2c_master.h"

typedef enum {
    GESTURE_NONE = 0,
    GESTURE_SWIPE_LEFT,
    GESTURE_SWIPE_RIGHT,
    GESTURE_SWIPE_UP,
    GESTURE_SWIPE_DOWN,
    GESTURE_TAP,
    GESTURE_DOUBLE_TAP,
} gesture_t;

typedef void (*gesture_callback_t)(gesture_t gesture);

esp_err_t touch_hal_init(i2c_master_bus_handle_t bus_handle, lv_disp_t *disp,
                         lv_indev_t **ret_indev);
void touch_hal_register_gesture_cb(gesture_callback_t cb);
void touch_hal_set_rotation(lv_disp_rot_t rotation);
void touch_hal_set_polling_enabled(bool enabled);

/**
 * Enable/disable image mode.
 * When active, all normal gestures are suppressed and only double-tap is emitted.
 */
void touch_hal_set_image_mode(bool active);

#endif
