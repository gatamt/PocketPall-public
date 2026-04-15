#include "imu_hal.h"
#include "app_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "imu_hal";

/* QMI8658 register map */
#define QMI_REG_WHO_AM_I     0x00
#define QMI_REG_CTRL1        0x02
#define QMI_REG_CTRL2        0x03
#define QMI_REG_CTRL3        0x04
#define QMI_REG_CTRL7        0x08
#define QMI_REG_AX_L         0x35
#define QMI_REG_RESET        0x60

/* Config: +-4g accel @125Hz, +-512dps gyro @112.1Hz */
#define QMI_CTRL1_ADDR_AI    0x40
#define QMI_CTRL2_CFG        0x16
#define QMI_CTRL3_CFG        0x56
#define QMI_CTRL7_ENABLE_AG  0x03
#define QMI_RESET_CMD        0xB0

#define QMI_ACCEL_SCALE_G    (4.0f / 32768.0f)
#define QMI_GYRO_SCALE_DPS   (512.0f / 32768.0f)

static i2c_master_dev_handle_t s_qmi_dev = NULL;
static bool s_ready = false;
static uint8_t s_active_addr = 0;

static esp_err_t qmi_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_qmi_dev, buf, sizeof(buf), 100);
}

static esp_err_t qmi_read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_qmi_dev, &reg, 1, value, 1, 100);
}

static esp_err_t qmi_read_bytes(uint8_t start_reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_qmi_dev, &start_reg, 1, buf, len, 100);
}

static esp_err_t qmi_try_attach(i2c_master_bus_handle_t bus_handle, uint8_t addr)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };

    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &cfg, &s_qmi_dev);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t whoami = 0;
    ret = qmi_read_reg(QMI_REG_WHO_AM_I, &whoami);
    if (ret != ESP_OK || whoami != QMI8658_WHOAMI_VAL) {
        i2c_master_bus_rm_device(s_qmi_dev);
        s_qmi_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    s_active_addr = addr;
    return ESP_OK;
}

esp_err_t imu_hal_init(i2c_master_bus_handle_t bus_handle)
{
    if (!bus_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ready) {
        return ESP_OK;
    }

    esp_err_t ret = qmi_try_attach(bus_handle, QMI8658_I2C_ADDR);
    if (ret != ESP_OK) {
        const uint8_t alt_addr = (QMI8658_I2C_ADDR == 0x6B) ? 0x6A : 0x6B;
        ret = qmi_try_attach(bus_handle, alt_addr);
    }
    if (ret != ESP_OK || !s_qmi_dev) {
        ESP_LOGW(TAG, "QMI8658 not detected on I2C");
        return ESP_ERR_NOT_FOUND;
    }

    /* Reset device and apply basic output configuration */
    ESP_RETURN_ON_ERROR(qmi_write_reg(QMI_REG_RESET, QMI_RESET_CMD), TAG, "QMI reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t whoami = 0;
    ESP_RETURN_ON_ERROR(qmi_read_reg(QMI_REG_WHO_AM_I, &whoami), TAG, "QMI WHO_AM_I read failed");
    if (whoami != QMI8658_WHOAMI_VAL) {
        ESP_LOGE(TAG, "Unexpected QMI8658 WHO_AM_I: 0x%02X", whoami);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_RETURN_ON_ERROR(qmi_write_reg(QMI_REG_CTRL1, QMI_CTRL1_ADDR_AI), TAG, "QMI CTRL1 write failed");
    ESP_RETURN_ON_ERROR(qmi_write_reg(QMI_REG_CTRL2, QMI_CTRL2_CFG), TAG, "QMI CTRL2 write failed");
    ESP_RETURN_ON_ERROR(qmi_write_reg(QMI_REG_CTRL3, QMI_CTRL3_CFG), TAG, "QMI CTRL3 write failed");
    ESP_RETURN_ON_ERROR(qmi_write_reg(QMI_REG_CTRL7, QMI_CTRL7_ENABLE_AG), TAG, "QMI CTRL7 write failed");

    s_ready = true;
    ESP_LOGI(TAG, "QMI8658 initialized at 0x%02X", s_active_addr);
    return ESP_OK;
}

bool imu_hal_is_ready(void)
{
    return s_ready;
}

esp_err_t imu_hal_read_sample(imu_sample_t *out_sample)
{
    if (!out_sample) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready || !s_qmi_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buf[12] = {0};
    esp_err_t ret = qmi_read_bytes(QMI_REG_AX_L, buf, sizeof(buf));
    if (ret != ESP_OK) {
        return ret;
    }

    int16_t raw_ax = (int16_t)((buf[1] << 8) | buf[0]);
    int16_t raw_ay = (int16_t)((buf[3] << 8) | buf[2]);
    int16_t raw_az = (int16_t)((buf[5] << 8) | buf[4]);
    int16_t raw_gx = (int16_t)((buf[7] << 8) | buf[6]);
    int16_t raw_gy = (int16_t)((buf[9] << 8) | buf[8]);
    int16_t raw_gz = (int16_t)((buf[11] << 8) | buf[10]);

    out_sample->ax_g = raw_ax * QMI_ACCEL_SCALE_G;
    out_sample->ay_g = raw_ay * QMI_ACCEL_SCALE_G;
    out_sample->az_g = raw_az * QMI_ACCEL_SCALE_G;
    out_sample->gx_dps = raw_gx * QMI_GYRO_SCALE_DPS;
    out_sample->gy_dps = raw_gy * QMI_GYRO_SCALE_DPS;
    out_sample->gz_dps = raw_gz * QMI_GYRO_SCALE_DPS;

    return ESP_OK;
}
