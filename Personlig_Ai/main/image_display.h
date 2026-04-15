#ifndef IMAGE_DISPLAY_H
#define IMAGE_DISPLAY_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * Initialize the image display module.
 * Allocates receive buffer in PSRAM.
 */
esp_err_t image_display_init(void);

/**
 * Begin receiving a new image from BLE.
 * Shows loading animation on screen.
 * @param width      Image width in pixels
 * @param height     Image height in pixels
 * @param total_size Total payload size in bytes.
 *                   If larger than one frame, payload must be a whole-number
 *                   multiple of (width * height * 2) and the last frame is rendered.
 */
esp_err_t image_display_begin(uint16_t width, uint16_t height, size_t total_size);

/**
 * Write a chunk of RGB565 image data received from BLE.
 * @param data   Raw RGB565 pixel data
 * @param len    Number of bytes in this chunk
 */
esp_err_t image_display_write_chunk(const uint8_t *data, size_t len);

/**
 * Image transfer complete. Display the assembled image on screen.
 * If called before all bytes are received, returns ESP_ERR_NOT_FINISHED.
 */
esp_err_t image_display_finish(void);

/**
 * Dismiss the currently displayed image and restore previous screen.
 */
void image_display_dismiss(void);

/**
 * Check if an image is currently being received or displayed.
 */
bool image_display_is_active(void);

/**
 * True while image bytes are still being received.
 */
bool image_display_is_receiving(void);

#endif /* IMAGE_DISPLAY_H */
