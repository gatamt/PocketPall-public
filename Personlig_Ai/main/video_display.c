#include "video_display.h"
#include "app_config.h"
#include "tjpgd_standalone.h"
#include "image_display.h"
#include "ui_manager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include <string.h>

static const char *TAG = "video_disp";

/* ---- PSRAM buffer sizes ---- */
#define VIDEO_JPEG_MAX_SIZE     (48 * 1024)     /* Max JPEG accumulation */
#define VIDEO_TJPGD_WORK_SIZE   3500            /* TJpgDec workspace */
#define VIDEO_FB_WIDTH          LCD_H_RES       /* 368 */
#define VIDEO_FB_HEIGHT         LCD_V_RES       /* 448 */
#define VIDEO_FB_SIZE           (VIDEO_FB_WIDTH * VIDEO_FB_HEIGHT * 2)  /* RGB565 = 322 KB */

/* ---- State ---- */
static bool     s_active = false;
static bool     s_stopped = false;       /* Explicitly stopped — reject auto-start */
static uint8_t *s_jpeg_buf = NULL;      /* JPEG accumulation buffer (PSRAM) */
static size_t   s_jpeg_len = 0;         /* Current accumulated bytes */
static uint16_t *s_rgb_fb = NULL;       /* RGB565 framebuffer (PSRAM) */
static uint8_t *s_tjpgd_work = NULL;    /* TJpgDec workspace (PSRAM) */
static lv_obj_t *s_video_screen = NULL; /* Persistent LVGL screen for video */
static lv_obj_t *s_prev_screen = NULL;  /* Saved screen to restore on stop */
static lv_obj_t *s_canvas = NULL;       /* LVGL image object on video screen */
static lv_img_dsc_t s_img_dsc;          /* Image descriptor for LVGL */
static uint32_t s_frame_count = 0;      /* Frame counter for stats */

/* ---- TJpgDec input callback ---- */

typedef struct {
    const uint8_t *data;
    size_t  len;
    size_t  pos;
} jpeg_input_t;

static size_t jpeg_infunc(JDEC *jd, uint8_t *buff, size_t nbyte)
{
    jpeg_input_t *inp = (jpeg_input_t *)jd->device;
    size_t remain = inp->len - inp->pos;
    if (nbyte > remain) nbyte = remain;

    if (buff) {
        memcpy(buff, inp->data + inp->pos, nbyte);
    }
    inp->pos += nbyte;
    return nbyte;
}

/* ---- TJpgDec output callback — write RGB565 MCU blocks to framebuffer ---- */

static int jpeg_outfunc(JDEC *jd, void *bitmap, JRECT *rect)
{
    (void)jd;
    uint16_t *src = (uint16_t *)bitmap;
    uint16_t w = rect->right - rect->left + 1;
    uint16_t h = rect->bottom - rect->top + 1;

    /* Clip to framebuffer bounds */
    if (rect->right >= VIDEO_FB_WIDTH || rect->bottom >= VIDEO_FB_HEIGHT) {
        return 1; /* Skip out-of-bounds MCUs */
    }

    for (uint16_t y = 0; y < h; y++) {
        uint16_t *dst = s_rgb_fb + (rect->top + y) * VIDEO_FB_WIDTH + rect->left;
        uint16_t *row = src + y * w;
        /* TJpgDec outputs native-endian RGB565 but LVGL is configured with
         * LV_COLOR_16_SWAP=y, so each pixel must be byte-swapped. */
        for (uint16_t x = 0; x < w; x++) {
            dst[x] = __builtin_bswap16(row[x]);
        }
    }

    return 1; /* Continue decompression */
}

/* ---- Public API ---- */

esp_err_t video_display_init(void)
{
    ESP_LOGI(TAG, "Video display module initialized");
    return ESP_OK;
}

esp_err_t video_display_start(void)
{
    s_stopped = false;  /* Clear explicit-stop flag on new start */

    if (s_active) {
        return ESP_OK; /* Already started */
    }

    /* Dismiss any active image display first (mutual exclusion) */
    if (image_display_is_active()) {
        image_display_dismiss();
    }

    /* Allocate PSRAM buffers */
    s_jpeg_buf = heap_caps_calloc(1, VIDEO_JPEG_MAX_SIZE, MALLOC_CAP_SPIRAM);
    s_rgb_fb = heap_caps_calloc(1, VIDEO_FB_SIZE, MALLOC_CAP_SPIRAM);
    s_tjpgd_work = heap_caps_calloc(1, VIDEO_TJPGD_WORK_SIZE, MALLOC_CAP_SPIRAM);

    if (!s_jpeg_buf || !s_rgb_fb || !s_tjpgd_work) {
        ESP_LOGE(TAG, "Failed to allocate video buffers");
        if (s_jpeg_buf)   { heap_caps_free(s_jpeg_buf);   s_jpeg_buf = NULL; }
        if (s_rgb_fb)     { heap_caps_free(s_rgb_fb);     s_rgb_fb = NULL; }
        if (s_tjpgd_work) { heap_caps_free(s_tjpgd_work); s_tjpgd_work = NULL; }
        return ESP_ERR_NO_MEM;
    }

    s_jpeg_len = 0;
    s_frame_count = 0;

    /* Create LVGL video screen with canvas */
    if (lvgl_port_lock(100)) {
        /* Save current screen so we can restore it on stop */
        s_prev_screen = lv_disp_get_scr_act(lv_disp_get_default());

        s_video_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_video_screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_video_screen, LV_OPA_COVER, 0);

        /* Set up image descriptor for RGB565 framebuffer */
        s_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        s_img_dsc.header.w = VIDEO_FB_WIDTH;
        s_img_dsc.header.h = VIDEO_FB_HEIGHT;
        s_img_dsc.data_size = VIDEO_FB_SIZE;
        s_img_dsc.data = (const uint8_t *)s_rgb_fb;

        s_canvas = lv_img_create(s_video_screen);
        lv_img_set_src(s_canvas, &s_img_dsc);
        lv_obj_center(s_canvas);

        lv_scr_load(s_video_screen);
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG, "Failed to acquire LVGL lock for video screen");
    }

    s_active = true;
    ESP_LOGI(TAG, "Video display started (JPEG=%dKB, FB=%dKB, Work=%dB)",
             VIDEO_JPEG_MAX_SIZE / 1024, VIDEO_FB_SIZE / 1024, VIDEO_TJPGD_WORK_SIZE);
    return ESP_OK;
}

esp_err_t video_display_feed(const uint8_t *data, size_t len, uint8_t flags)
{
    if (!s_active) {
        return ESP_ERR_INVALID_STATE;  /* Not started or explicitly stopped */
    }

    /* SOF flag: reset JPEG accumulation */
    if (flags & VIDEO_FLAG_SOF) {
        s_jpeg_len = 0;
    }

    /* Append data to JPEG buffer */
    if (s_jpeg_len + len > VIDEO_JPEG_MAX_SIZE) {
        ESP_LOGW(TAG, "JPEG overflow: %u + %u > %u, dropping frame",
                 (unsigned)s_jpeg_len, (unsigned)len, VIDEO_JPEG_MAX_SIZE);
        s_jpeg_len = 0;
        return ESP_ERR_NO_MEM;
    }

    memcpy(s_jpeg_buf + s_jpeg_len, data, len);
    s_jpeg_len += len;

    /* EOF flag: decode and display */
    if (flags & VIDEO_FLAG_EOF) {
        jpeg_input_t inp = {
            .data = s_jpeg_buf,
            .len  = s_jpeg_len,
            .pos  = 0,
        };

        JDEC jdec;
        JRESULT rc = jd_prepare(&jdec, jpeg_infunc, s_tjpgd_work, VIDEO_TJPGD_WORK_SIZE, &inp);
        if (rc != JDR_OK) {
            ESP_LOGW(TAG, "jd_prepare failed: %d (jpeg_len=%u)", rc, (unsigned)s_jpeg_len);
            s_jpeg_len = 0;
            return ESP_FAIL;
        }

        rc = jd_decomp(&jdec, jpeg_outfunc, 0);
        if (rc != JDR_OK) {
            ESP_LOGW(TAG, "jd_decomp failed: %d", rc);
            s_jpeg_len = 0;
            return ESP_FAIL;
        }

        /* Invalidate LVGL canvas to trigger redraw */
        if (lvgl_port_lock(10)) {
            if (s_canvas) {
                lv_obj_invalidate(s_canvas);
            }
            lvgl_port_unlock();
        }

        s_frame_count++;
        /* Stats every 30 frames */
        if ((s_frame_count % 30) == 0) {
            ESP_LOGI(TAG, "Video frame #%lu, JPEG size=%u bytes",
                     (unsigned long)s_frame_count, (unsigned)s_jpeg_len);
        }

        s_jpeg_len = 0;
    }

    return ESP_OK;
}

void video_display_stop(void)
{
    if (!s_active) return;

    s_active = false;
    s_stopped = true;

    /* Restore previous screen (LVGL main screen) */
    if (lvgl_port_lock(100)) {
        if (s_video_screen) {
            /* Detach framebuffer from canvas before freeing memory */
            if (s_canvas) {
                lv_img_set_src(s_canvas, NULL);
            }

            lv_disp_t *disp = lv_disp_get_default();

            /* Restore saved screen (or create fallback) */
            if (s_prev_screen) {
                lv_scr_load(s_prev_screen);
            } else {
                lv_obj_t *blank = lv_obj_create(NULL);
                lv_obj_set_style_bg_color(blank, lv_color_black(), 0);
                lv_obj_set_style_bg_opa(blank, LV_OPA_COVER, 0);
                lv_scr_load(blank);
            }

            lv_obj_del(s_video_screen);

            /*
             * After lv_scr_load(prev) + lv_obj_del(s_video_screen):
             * disp->prev_scr still points to the deleted video screen.
             * Clear it to prevent the LVGL refresh timer from accessing
             * freed memory (which causes LoadProhibited crash).
             */
            disp->prev_scr = NULL;

            s_video_screen = NULL;
            s_canvas = NULL;
            s_prev_screen = NULL;
        }
        lvgl_port_unlock();
    }

    /* Free PSRAM buffers */
    if (s_jpeg_buf)   { heap_caps_free(s_jpeg_buf);   s_jpeg_buf = NULL; }
    if (s_rgb_fb)     { heap_caps_free(s_rgb_fb);     s_rgb_fb = NULL; }
    if (s_tjpgd_work) { heap_caps_free(s_tjpgd_work); s_tjpgd_work = NULL; }

    s_jpeg_len = 0;

    ESP_LOGI(TAG, "Video display stopped (displayed %lu frames)", (unsigned long)s_frame_count);
    s_frame_count = 0;
}

bool video_display_is_active(void)
{
    return s_active;
}
