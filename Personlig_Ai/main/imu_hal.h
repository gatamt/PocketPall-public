#ifndef IMU_HAL_H
#define IMU_HAL_H

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdbool.h>

typedef struct {
    float ax_g;
    float ay_g;
    float az_g;
    float gx_dps;
    float gy_dps;
    float gz_dps;
} imu_sample_t;

esp_err_t imu_hal_init(i2c_master_bus_handle_t bus_handle);
bool imu_hal_is_ready(void);
esp_err_t imu_hal_read_sample(imu_sample_t *out_sample);

#endif
