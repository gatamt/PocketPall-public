#include "display_hal.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_lvgl_port.h"
#include "driver/spi_master.h"
#include <string.h>

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_panel_io = NULL;

/* SH8601 custom init commands (must be at file scope per driver docs) */
static const uint8_t s_cmd_11_data[] = {0x00};
static const uint8_t s_cmd_44_data[] = {0x01, 0xD1};
static const uint8_t s_cmd_35_data[] = {0x00};
static const uint8_t s_cmd_53_data[] = {0x20};
static const uint8_t s_cmd_2a_data[] = {0x00, 0x00, 0x01, 0x6F};
static const uint8_t s_cmd_2b_data[] = {0x00, 0x00, 0x01, 0xBF};
static const uint8_t s_cmd_51_off_data[] = {0x00};
static const uint8_t s_cmd_29_data[] = {0x00};
static const uint8_t s_cmd_51_on_data[] = {0xFF};

static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, s_cmd_11_data,     0, 120},   /* Sleep Out + 120ms */
    {0x44, s_cmd_44_data,     2, 0},     /* Set Tear Scanline */
    {0x35, s_cmd_35_data,     1, 0},     /* Tearing Effect On */
    {0x53, s_cmd_53_data,     1, 10},    /* Write CTRL Display */
    {0x2A, s_cmd_2a_data,     4, 0},     /* Column Address 0-367 */
    {0x2B, s_cmd_2b_data,     4, 0},     /* Page Address 0-447 */
    {0x51, s_cmd_51_off_data, 1, 10},    /* Brightness = 0 */
    {0x29, s_cmd_29_data,     0, 10},    /* Display ON */
    {0x51, s_cmd_51_on_data,  1, 0},     /* Brightness = full */
};

/* SH8601 requires coordinates divisible by 2 */
static void sh8601_rounder_cb(lv_area_t *area)
{
    area->x1 = area->x1 & ~1;
    area->y1 = area->y1 & ~1;
    area->x2 = area->x2 | 1;
    area->y2 = area->y2 | 1;
}

esp_err_t display_hal_init(lv_disp_t **ret_disp)
{
    ESP_LOGI(TAG, "Initializing SH8601 AMOLED display...");

    /* Step 1: Initialize QSPI bus */
    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_QSPI_CLK,
        .data0_io_num = LCD_QSPI_D0,
        .data1_io_num = LCD_QSPI_D1,
        .data2_io_num = LCD_QSPI_D2,
        .data3_io_num = LCD_QSPI_D3,
        .max_transfer_sz = LCD_BUF_SIZE,
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* Step 2: Panel IO configuration for QSPI */
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_QSPI_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {
            .quad_mode = true,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                              &io_config, &s_panel_io));

    /* Step 3: SH8601 panel driver with custom init commands */
    sh8601_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,  /* Reset via IO expander */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BPP,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(s_panel_io, &panel_config, &s_panel));

    /* Step 4: Initialize and turn on display */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    /* Match Waveshare reference orientation for this panel. */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, LCD_MIRROR_X, LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    /* Step 5: Register with LVGL via esp_lvgl_port */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_panel_io,
        .panel_handle = s_panel,
        .buffer_size = LCD_H_RES * LCD_BUF_LINES,  /* Size in PIXELS, not bytes */
        .double_buffer = true,
        /* Use a dedicated DMA transfer buffer to avoid SPI queue ENOMEM when LVGL draw
         * buffers live in PSRAM. */
        .trans_size = LCD_H_RES * LCD_TRANS_LINES,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rounder_cb = sh8601_rounder_cb,
        .rotation = {
            .swap_xy = false,
            .mirror_x = LCD_MIRROR_X,
            .mirror_y = LCD_MIRROR_Y,
        },
        .flags = {
            .buff_spiram = true,
            .sw_rotate = true,
        },
    };
    *ret_disp = lvgl_port_add_disp(&disp_cfg);
    if (*ret_disp == NULL) {
        ESP_LOGE(TAG, "Failed to add LVGL display");
        return ESP_FAIL;
    }

    /* Step 6: Set display background to black (needed for smooth rotation fade on AMOLED) */
    lvgl_port_lock(0);
    lv_disp_set_bg_color(*ret_disp, lv_color_black());
    lv_disp_set_bg_opa(*ret_disp, LV_OPA_COVER);
    lvgl_port_unlock();

    /* Step 7: Set initial brightness to 80% */
    display_hal_set_brightness(80);

    ESP_LOGI(TAG, "Display initialized: %dx%d", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

esp_err_t display_hal_set_brightness(uint8_t percent)
{
    if (!s_panel_io) return ESP_ERR_INVALID_STATE;
    uint8_t brightness = (uint8_t)((uint16_t)percent * 255 / 100);
    /* QSPI mode: command must include opcode (0x02) in bits 31-24 and register in bits 15-8 */
    int qspi_cmd = (0x02 << 24) | (0x51 << 8);
    return esp_lcd_panel_io_tx_param(s_panel_io, qspi_cmd, &brightness, 1);
}
