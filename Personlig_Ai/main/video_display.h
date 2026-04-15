#ifndef VIDEO_DISPLAY_H
#define VIDEO_DISPLAY_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Video frame flags (in L2CAP frame flags byte) */
#define VIDEO_FLAG_SOF  0x01    /* Start of JPEG frame */
#define VIDEO_FLAG_EOF  0x02    /* End of JPEG frame */

/**
 * Initialize video display module (no allocation yet).
 */
esp_err_t video_display_init(void);

/**
 * Start video mode: allocate PSRAM buffers, create LVGL screen.
 * Call this when receiving the first video frame or "video_start" command.
 */
esp_err_t video_display_start(void);

/**
 * Feed video JPEG data. Fragments arrive with SOF/EOF flags.
 * SOF resets accumulation, EOF triggers JPEG decode and display.
 */
esp_err_t video_display_feed(const uint8_t *data, size_t len, uint8_t flags);

/**
 * Stop video mode: free buffers, restore previous LVGL screen.
 */
void video_display_stop(void);

/**
 * Check if video mode is currently active.
 */
bool video_display_is_active(void);

#endif /* VIDEO_DISPLAY_H */
