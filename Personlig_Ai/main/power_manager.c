#include "power_manager.h"
#include "app_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "power_mgr";

static i2c_master_dev_handle_t s_axp2101_dev = NULL;
static i2c_master_dev_handle_t s_tca9554_dev = NULL;
static uint8_t s_io_output_state = 0x00;

/* AXP2101 registers used */
#define AXP2101_REG_STATUS0             0x00
#define AXP2101_REG_STATUS1             0x01
#define AXP2101_REG_CHIP_ID             0x03
#define AXP2101_REG_COMMON_CONFIG       0x10
#define AXP2101_REG_VINDPM              0x15
#define AXP2101_REG_VBUS_CUR_LIMIT     0x16
#define AXP2101_REG_CHG_GAUGE_WDT_CTRL  0x18
#define AXP2101_REG_BOOT_REASON         0x20
#define AXP2101_REG_ADC_CTRL            0x30
#define AXP2101_REG_PRE_CHG_CURRENT     0x61
#define AXP2101_REG_CHG_CURRENT_CTRL    0x62
#define AXP2101_REG_TERM_CHG_CURRENT    0x63
#define AXP2101_REG_CHG_TARGET_VOLT     0x64
#define AXP2101_REG_LDO_ONOFF_CTRL0     0x90
#define AXP2101_REG_LDO_VOL_BLDO1       0x96
#define AXP2101_REG_LDO_VOL_BLDO2       0x97
#define AXP2101_REG_LDO_VOL_DLDO1       0x99
#define AXP2101_REG_FUEL_GAUGE          0xA4

/* TCA9554 registers used */
#define TCA9554_REG_INPUT_PORT          0x00
#define TCA9554_REG_OUTPUT_PORT         0x01
#define TCA9554_REG_CONFIG              0x03

/* ---- I2C helpers ---- */

static esp_err_t i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    esp_err_t ret = i2c_master_transmit(dev, buf, sizeof(buf), 100);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C write failed (reg=0x%02X, val=0x%02X): %s",
                 reg, val, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t i2c_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    esp_err_t ret = i2c_master_transmit_receive(dev, &reg, 1, val, 1, 100);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C read failed (reg=0x%02X): %s", reg, esp_err_to_name(ret));
    }
    return ret;
}

/* AXP2101 LDO voltage register: lower 5 bits, 100mV steps starting at 500mV */
static esp_err_t axp2101_set_ldo_voltage(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t millivolt)
{
    if (millivolt < 500 || millivolt > 3500 || (millivolt % 100) != 0) {
        ESP_LOGE(TAG, "Invalid AXP2101 LDO voltage: %u mV (reg 0x%02X)", millivolt, reg);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t reg_val = 0;
    ESP_RETURN_ON_ERROR(i2c_read_reg(dev, reg, &reg_val), TAG, "Read LDO voltage reg failed");
    reg_val &= 0xE0;
    reg_val |= (uint8_t)((millivolt - 500) / 100);
    return i2c_write_reg(dev, reg, reg_val);
}

/* ---- Public API ---- */

esp_err_t power_manager_init_i2c(i2c_master_bus_handle_t *ret_bus_handle)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA,
        .scl_io_num = I2C_MASTER_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_config, ret_bus_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "I2C bus initialized (SDA=%d, SCL=%d, %dHz)",
                 I2C_MASTER_SDA, I2C_MASTER_SCL, I2C_MASTER_FREQ_HZ);
    }
    return ret;
}

esp_err_t power_manager_init_pmic(i2c_master_bus_handle_t bus_handle)
{
    ESP_RETURN_ON_FALSE(bus_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid I2C bus handle");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_I2C_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &s_axp2101_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add AXP2101 device");
        return ret;
    }

    uint8_t chip_id = 0;
    ESP_RETURN_ON_ERROR(i2c_read_reg(s_axp2101_dev, AXP2101_REG_CHIP_ID, &chip_id), TAG,
                        "AXP2101 probe failed");
    ESP_LOGI(TAG, "AXP2101 detected (IC type reg: 0x%02X)", chip_id);

    /* --- Set voltages BEFORE enabling outputs --- */
    ESP_RETURN_ON_ERROR(axp2101_set_ldo_voltage(s_axp2101_dev, AXP2101_REG_LDO_VOL_BLDO1, 3300), TAG,
                        "Set BLDO1 voltage failed");
    ESP_RETURN_ON_ERROR(axp2101_set_ldo_voltage(s_axp2101_dev, AXP2101_REG_LDO_VOL_BLDO2, 3300), TAG,
                        "Set BLDO2 voltage failed");
    ESP_RETURN_ON_ERROR(axp2101_set_ldo_voltage(s_axp2101_dev, AXP2101_REG_LDO_VOL_DLDO1, 3300), TAG,
                        "Set DLDO1 voltage failed");

    /* Enable BLDO1 + BLDO2 + DLDO1 without clobbering other rail enables */
    uint8_t ldo_onoff = 0;
    ESP_RETURN_ON_ERROR(i2c_read_reg(s_axp2101_dev, AXP2101_REG_LDO_ONOFF_CTRL0, &ldo_onoff), TAG,
                        "Read LDO on/off register failed");
    ldo_onoff |= ((1U << 7) | (1U << 5) | (1U << 4));
    ESP_RETURN_ON_ERROR(i2c_write_reg(s_axp2101_dev, AXP2101_REG_LDO_ONOFF_CTRL0, ldo_onoff), TAG,
                        "Enable AXP2101 LDO rails failed");

    /* Wait for power rails to stabilize */
    vTaskDelay(pdMS_TO_TICKS(20));

    /* --- Power path configuration for USB+Battery coexistence --- */

    /* VINDPM: Input voltage limit 4.36V (bits 3:0 = 6) */
    uint8_t vindpm = 0;
    ESP_RETURN_ON_ERROR(i2c_read_reg(s_axp2101_dev, AXP2101_REG_VINDPM, &vindpm), TAG,
                        "Read VINDPM register failed");
    vindpm = (vindpm & 0xF0) | 0x06;
    ESP_RETURN_ON_ERROR(i2c_write_reg(s_axp2101_dev, AXP2101_REG_VINDPM, vindpm), TAG,
                        "Set VINDPM failed");

    /* VBUS input current limit: 500mA (bits 2:0 = 1, USB 2.0 safe) */
    uint8_t vbus_cur = 0;
    ESP_RETURN_ON_ERROR(i2c_read_reg(s_axp2101_dev, AXP2101_REG_VBUS_CUR_LIMIT, &vbus_cur), TAG,
                        "Read VBUS current limit failed");
    vbus_cur = (vbus_cur & 0xF8) | 0x01;
    ESP_RETURN_ON_ERROR(i2c_write_reg(s_axp2101_dev, AXP2101_REG_VBUS_CUR_LIMIT, vbus_cur), TAG,
                        "Set VBUS current limit failed");

    /* Charge target voltage: 4.20V (bits 2:0 = 2, standard LiPo) */
    uint8_t chg_volt = 0;
    ESP_RETURN_ON_ERROR(i2c_read_reg(s_axp2101_dev, AXP2101_REG_CHG_TARGET_VOLT, &chg_volt), TAG,
                        "Read charge target voltage failed");
    chg_volt = (chg_volt & 0xF8) | 0x02;
    ESP_RETURN_ON_ERROR(i2c_write_reg(s_axp2101_dev, AXP2101_REG_CHG_TARGET_VOLT, chg_volt), TAG,
                        "Set charge target voltage failed");

    /* Pre-charge current: 50mA */
    ESP_RETURN_ON_ERROR(i2c_write_reg(s_axp2101_dev, AXP2101_REG_PRE_CHG_CURRENT, 0x02), TAG,
                        "Set pre-charge current failed");

    /* Termination current: 25mA */
    ESP_RETURN_ON_ERROR(i2c_write_reg(s_axp2101_dev, AXP2101_REG_TERM_CHG_CURRENT, 0x01), TAG,
                        "Set termination current failed");

    /* Enable ADC channels: battery + TS + VBUS + system voltage */
    ESP_RETURN_ON_ERROR(i2c_write_reg(s_axp2101_dev, AXP2101_REG_ADC_CTRL, 0x0F), TAG,
                        "Enable ADC channels failed");

    /* Set charging current to 200mA */
    ESP_RETURN_ON_ERROR(i2c_write_reg(s_axp2101_dev, AXP2101_REG_CHG_CURRENT_CTRL, 0x04), TAG,
                        "Set charging current failed");

    /* Enable charging + fuel gauge (preserve other bits) */
    uint8_t chg_cfg = 0;
    ESP_RETURN_ON_ERROR(i2c_read_reg(s_axp2101_dev, AXP2101_REG_CHG_GAUGE_WDT_CTRL, &chg_cfg), TAG,
                        "Read charger config register failed");
    chg_cfg |= 0x03;  /* bit 1 = charging enable, bit 0 = fuel gauge enable */
    ESP_RETURN_ON_ERROR(i2c_write_reg(s_axp2101_dev, AXP2101_REG_CHG_GAUGE_WDT_CTRL, chg_cfg), TAG,
                        "Enable charging/gauge failed");

    ESP_LOGI(TAG, "AXP2101 PMIC initialized (power path + charging configured)");
    return ESP_OK;
}

esp_err_t power_manager_init_io_expander(i2c_master_bus_handle_t bus_handle)
{
    ESP_RETURN_ON_FALSE(bus_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid I2C bus handle");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9554_I2C_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &s_tca9554_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add TCA9554 device");
        return ret;
    }

    /* Read current TCA9554 state first, then only touch relevant bits. */
    uint8_t cfg_reg = 0xFF;
    uint8_t out_reg = 0xFF;
    ESP_RETURN_ON_ERROR(i2c_read_reg(s_tca9554_dev, TCA9554_REG_CONFIG, &cfg_reg), TAG,
                        "Read TCA9554 config failed");
    ESP_RETURN_ON_ERROR(i2c_read_reg(s_tca9554_dev, TCA9554_REG_OUTPUT_PORT, &out_reg), TAG,
                        "Read TCA9554 output state failed");

    /* EXIO0/1/2 (+ EXIO7 for SD_CS) output, EXIO4/5 input, preserve other pins */
    cfg_reg &= (uint8_t)~((1U << 0) | (1U << 1) | (1U << 2) | (1U << 7));
    cfg_reg |= (uint8_t)((1U << 4) | (1U << 5));
    ESP_RETURN_ON_ERROR(i2c_write_reg(s_tca9554_dev, TCA9554_REG_CONFIG, cfg_reg), TAG,
                        "Configure TCA9554 pin directions failed");

    /* Keep SD_CS high and assert reset/enable/touch-reset low */
    s_io_output_state = (uint8_t)(out_reg | (1U << 7));
    s_io_output_state &= (uint8_t)~((1U << 0) | (1U << 1) | (1U << 2));
    ESP_RETURN_ON_ERROR(i2c_write_reg(s_tca9554_dev, TCA9554_REG_OUTPUT_PORT, s_io_output_state), TAG,
                        "Assert display/touch reset failed");

    /* Hold reset for 200ms (Waveshare reference) */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Release reset/enable/touch-reset */
    s_io_output_state |= (uint8_t)((1U << 0) | (1U << 1) | (1U << 2));
    ESP_RETURN_ON_ERROR(i2c_write_reg(s_tca9554_dev, TCA9554_REG_OUTPUT_PORT, s_io_output_state), TAG,
                        "Release display/touch reset failed");

    /* Wait for SH8601 + FT3168 to stabilize */
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "TCA9554 IO expander initialized");
    return ESP_OK;
}

esp_err_t power_manager_io_set_pin(uint8_t pin, bool level)
{
    if (pin > 7 || !s_tca9554_dev) return ESP_ERR_INVALID_ARG;

    if (level) {
        s_io_output_state |= (1 << pin);
    } else {
        s_io_output_state &= ~(1 << pin);
    }
    return i2c_write_reg(s_tca9554_dev, TCA9554_REG_OUTPUT_PORT, s_io_output_state);
}

esp_err_t power_manager_io_get_pin(uint8_t pin, bool *level)
{
    if (pin > 7 || !level || !s_tca9554_dev) return ESP_ERR_INVALID_ARG;

    uint8_t input_reg = 0;
    esp_err_t ret = i2c_read_reg(s_tca9554_dev, TCA9554_REG_INPUT_PORT, &input_reg);
    if (ret != ESP_OK) {
        return ret;
    }
    *level = ((input_reg >> pin) & 0x01) ? true : false;
    return ESP_OK;
}

esp_err_t power_manager_get_battery_mv(uint16_t *voltage_mv)
{
    if (!s_axp2101_dev || !voltage_mv) return ESP_ERR_INVALID_ARG;

    uint8_t high, low;
    esp_err_t ret = i2c_read_reg(s_axp2101_dev, 0x34, &high);
    if (ret != ESP_OK) return ret;
    ret = i2c_read_reg(s_axp2101_dev, 0x35, &low);
    if (ret != ESP_OK) return ret;

    /* AXP2101 battery voltage: 14-bit value, LSB = 1mV */
    *voltage_mv = ((uint16_t)high << 8) | low;
    return ESP_OK;
}

esp_err_t power_manager_get_battery_percent(uint8_t *percent)
{
    if (!percent) return ESP_ERR_INVALID_ARG;
    if (!s_axp2101_dev) return ESP_ERR_INVALID_STATE;

    /* Use AXP2101 fuel gauge register (0xA4) for accurate SoC */
    uint8_t soc = 0;
    esp_err_t ret = i2c_read_reg(s_axp2101_dev, AXP2101_REG_FUEL_GAUGE, &soc);
    if (ret != ESP_OK) {
        /* Fallback to voltage-based estimation */
        uint16_t mv;
        ret = power_manager_get_battery_mv(&mv);
        if (ret != ESP_OK) return ret;
        if (mv <= 3300) {
            *percent = 0;
        } else if (mv >= 4200) {
            *percent = 100;
        } else {
            *percent = (uint8_t)((mv - 3300) * 100 / 900);
        }
        return ESP_OK;
    }

    if (soc > 100) soc = 100;
    *percent = soc;
    return ESP_OK;
}

bool power_manager_is_vbus_present(void)
{
    if (!s_axp2101_dev) return false;
    uint8_t status0 = 0;
    if (i2c_read_reg(s_axp2101_dev, AXP2101_REG_STATUS0, &status0) != ESP_OK) return false;
    return (status0 & (1U << 5)) != 0;  /* Bit 5: VBUS present */
}

bool power_manager_is_charging(void)
{
    if (!s_axp2101_dev) return false;
    uint8_t status1 = 0;
    if (i2c_read_reg(s_axp2101_dev, AXP2101_REG_STATUS1, &status1) != ESP_OK) return false;
    /* Bits 6:5 = charging state: 01 = charging */
    return ((status1 >> 5) & 0x03) == 1;
}

uint8_t power_manager_get_boot_reason(void)
{
    if (!s_axp2101_dev) return 0;
    uint8_t reason = 0;
    i2c_read_reg(s_axp2101_dev, AXP2101_REG_BOOT_REASON, &reason);
    return reason;
}

void power_manager_shutdown(void)
{
    if (!s_axp2101_dev) {
        ESP_LOGE(TAG, "AXP2101 not initialized, cannot shut down");
        return;
    }

    ESP_LOGW(TAG, "Shutting down via AXP2101...");

    /* AXP2101 REG 0x10 bit 0: set to 1 to power off */
    uint8_t val = 0;
    if (i2c_read_reg(s_axp2101_dev, AXP2101_REG_COMMON_CONFIG, &val) == ESP_OK) {
        val |= 0x01;
        i2c_write_reg(s_axp2101_dev, AXP2101_REG_COMMON_CONFIG, val);
    }

    /* Should not reach here — PMIC cuts power immediately */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}
