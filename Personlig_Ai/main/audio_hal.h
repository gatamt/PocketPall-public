#ifndef AUDIO_HAL_H
#define AUDIO_HAL_H

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdint.h>
#include <stdbool.h>

typedef void (*vad_callback_t)(bool speech_active);

/* Streaming recording callback: delivers DSP-processed mono PCM chunks */
typedef void (*audio_stream_cb_t)(const int16_t *pcm, size_t bytes);

/* Standard (batch) recording/playback API */
esp_err_t audio_hal_init(i2c_master_bus_handle_t bus_handle);
esp_err_t audio_hal_start_recording(void);
esp_err_t audio_hal_stop_recording(void);
esp_err_t audio_hal_get_recording(const int16_t **data, size_t *length_bytes);
esp_err_t audio_hal_play_audio(const int16_t *data, size_t length_bytes, uint32_t sample_rate);
esp_err_t audio_hal_stop_playback(void);
bool audio_hal_is_playing(void);
bool audio_hal_is_recording(void);
esp_err_t audio_hal_set_volume(uint8_t volume);
void audio_hal_register_vad_cb(vad_callback_t cb);
uint16_t audio_hal_get_current_energy(void);

/* Streaming recording API (for Live API) */
esp_err_t audio_hal_start_streaming(audio_stream_cb_t cb);
esp_err_t audio_hal_stop_streaming(void);
bool audio_hal_is_streaming(void);

/* Streaming playback API (ring buffer) */
esp_err_t audio_hal_start_stream_playback(uint32_t sample_rate);
esp_err_t audio_hal_stream_write(const int16_t *data, size_t bytes);
esp_err_t audio_hal_stream_end(void);
esp_err_t audio_hal_stream_flush(void);
bool audio_hal_is_stream_playing(void);

#endif
