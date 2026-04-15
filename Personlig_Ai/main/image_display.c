#include "image_display.h"
#include "app_config.h"
#include "ui_manager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include <string.h>

static const char *TAG = "image_disp";

/* Receive buffer for assembling RGB565 image from BLE chunks */
static uint8_t *s_img_buf = NULL;
static uint16_t s_img_w = 0;
static uint16_t s_img_h = 0;
static size_t s_img_frame_size = 0;
static size_t s_img_total_size = 0;
static size_t s_img_received = 0;
static size_t s_img_frame_off = 0;
static size_t s_img_frame_count = 1;
static bool s_img_shown = false;
static volatile bool s_active = false;
static volatile bool s_receiving = false;

static esp_err_t image_display_show_if_complete(void)
{
    if (!s_img_buf || !s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_img_received < s_img_total_size) {
        return ESP_ERR_NOT_FINISHED;
    }
    if (!s_img_shown) {
        ui_manager_show_image((const uint16_t *)s_img_buf, s_img_w, s_img_h);
        s_img_shown = true;
        ESP_LOGI(TAG, "Image displayed: %ux%u (%u bytes)",
                 (unsigned)s_img_w, (unsigned)s_img_h, (unsigned)s_img_total_size);
    }
    return ESP_OK;
}

esp_err_t image_display_init(void)
{
    ESP_LOGI(TAG, "Image display module initialized");
    return ESP_OK;
}

esp_err_t image_display_begin(uint16_t width, uint16_t height, size_t total_size)
{
    if (width == 0 || height == 0 || total_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (width > LCD_H_RES || height > LCD_V_RES) {
        ESP_LOGE(TAG, "Unsupported image dimensions: %ux%u (panel is %ux%u)",
                 (unsigned)width, (unsigned)height, (unsigned)LCD_H_RES, (unsigned)LCD_V_RES);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t pixel_count = (size_t)width * (size_t)height;
    if (pixel_count == 0 || pixel_count > (SIZE_MAX / sizeof(uint16_t))) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t frame_size = pixel_count * sizeof(uint16_t);
    if (total_size < frame_size) {
        ESP_LOGE(TAG, "Image payload too small: total=%u frame=%u",
                 (unsigned)total_size, (unsigned)frame_size);
        return ESP_ERR_INVALID_SIZE;
    }
    if ((total_size % frame_size) != 0) {
        ESP_LOGE(TAG, "Image payload is not whole RGB565 frames: total=%u frame=%u",
                 (unsigned)total_size, (unsigned)frame_size);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Free any previous buffer */
    if (s_img_buf) {
        heap_caps_free(s_img_buf);
        s_img_buf = NULL;
    }

    /* Keep only one frame in RAM; if multiple frames arrive, render the last one. */
    s_img_buf = heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM);
    if (!s_img_buf) {
        ESP_LOGE(TAG, "Failed to allocate image frame buffer (%u bytes)", (unsigned)frame_size);
        return ESP_ERR_NO_MEM;
    }

    s_img_w = width;
    s_img_h = height;
    s_img_frame_size = frame_size;
    s_img_total_size = total_size;
    s_img_received = 0;
    s_img_frame_off = 0;
    s_img_frame_count = total_size / frame_size;
    s_img_shown = false;
    s_active = true;
    s_receiving = true;

    /* Show loading animation */
    ui_manager_show_image_loading();

    if (s_img_frame_count > 1) {
        ESP_LOGW(TAG, "Image stream has %u frames; displaying last frame only", (unsigned)s_img_frame_count);
    }
    ESP_LOGI(TAG, "Image receive started: %ux%u, total=%u bytes, frame=%u bytes",
             (unsigned)width, (unsigned)height, (unsigned)total_size, (unsigned)frame_size);
    return ESP_OK;
}

esp_err_t image_display_write_chunk(const uint8_t *data, size_t len)
{
    if (!s_active || !s_img_buf) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_receiving) {
        /* Ignore late bytes after payload has completed. */
        return ESP_OK;
    }
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_img_received >= s_img_total_size) {
        return ESP_OK;
    }

    size_t remaining = s_img_total_size - s_img_received;
    size_t to_copy = len < remaining ? len : remaining;
    size_t src_off = 0;

    while (to_copy > 0) {
        size_t frame_remaining = s_img_frame_size - s_img_frame_off;
        size_t copy_now = (to_copy < frame_remaining) ? to_copy : frame_remaining;
        memcpy(s_img_buf + s_img_frame_off, data + src_off, copy_now);
        s_img_frame_off += copy_now;
        if (s_img_frame_off >= s_img_frame_size) {
            s_img_frame_off = 0;
        }
        src_off += copy_now;
        to_copy -= copy_now;
        s_img_received += copy_now;
    }

    if (len > src_off) {
        ESP_LOGW(TAG, "Dropping %u excess image bytes after declared payload",
                 (unsigned)(len - src_off));
    }

    if (s_img_received >= s_img_total_size) {
        s_receiving = false;
        return image_display_show_if_complete();
    }

    return ESP_OK;
}

esp_err_t image_display_finish(void)
{
    if (!s_active || !s_img_buf) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_img_received < s_img_total_size) {
        ESP_LOGW(TAG, "image_done received early: %u / %u bytes",
                 (unsigned)s_img_received, (unsigned)s_img_total_size);
        return ESP_ERR_NOT_FINISHED;
    }

    s_receiving = false;
    return image_display_show_if_complete();
}

void image_display_dismiss(void)
{
    ui_manager_dismiss_image();

    if (s_img_buf) {
        heap_caps_free(s_img_buf);
        s_img_buf = NULL;
        ESP_LOGI(TAG, "Image buffer freed");
    }

    s_img_w = 0;
    s_img_h = 0;
    s_img_frame_size = 0;
    s_img_total_size = 0;
    s_img_received = 0;
    s_img_frame_off = 0;
    s_img_frame_count = 1;
    s_img_shown = false;
    s_active = false;
    s_receiving = false;
}

bool image_display_is_active(void)
{
    return s_active;
}

bool image_display_is_receiving(void)
{
    return s_receiving;
}
