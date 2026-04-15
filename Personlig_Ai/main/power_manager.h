#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include "esp_err.h"
#include "driver/i2c_master.h"

esp_err_t power_manager_init_i2c(i2c_master_bus_handle_t *ret_bus_handle);
esp_err_t power_manager_init_pmic(i2c_master_bus_handle_t bus_handle);
esp_err_t power_manager_init_io_expander(i2c_master_bus_handle_t bus_handle);
esp_err_t power_manager_io_set_pin(uint8_t pin, bool level);
esp_err_t power_manager_io_get_pin(uint8_t pin, bool *level);
esp_err_t power_manager_get_battery_mv(uint16_t *voltage_mv);
esp_err_t power_manager_get_battery_percent(uint8_t *percent);
bool power_manager_is_vbus_present(void);
bool power_manager_is_charging(void);
uint8_t power_manager_get_boot_reason(void);
void power_manager_shutdown(void);

#endif
