#ifndef DISPLAY_HAL_H
#define DISPLAY_HAL_H

#include "esp_err.h"
#include "lvgl.h"

esp_err_t display_hal_init(lv_disp_t **ret_disp);
esp_err_t display_hal_set_brightness(uint8_t percent);

#endif
