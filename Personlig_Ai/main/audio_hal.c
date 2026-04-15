#include "audio_hal.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <math.h>

static const char *TAG = "audio";

/* Codec device handles */
static esp_codec_dev_handle_t s_codec_dev = NULL;
static const audio_codec_data_if_t *s_data_if = NULL;
static const audio_codec_ctrl_if_t *s_ctrl_if = NULL;
static const audio_codec_if_t *s_codec_if = NULL;
static const audio_codec_gpio_if_t *s_gpio_if = NULL;

/* I2S channel handles */
static i2s_chan_handle_t s_i2s_tx_chan = NULL;
static i2s_chan_handle_t s_i2s_rx_chan = NULL;

/* Recording buffer in PSRAM */
static int16_t *s_rec_buffer = NULL;
static size_t s_rec_length = 0;        /* bytes recorded */
static volatile bool s_recording = false;
static volatile bool s_playing = false;
static volatile uint16_t s_current_energy = 0;

/* VAD callback */
static vad_callback_t s_vad_cb = NULL;

/* DSP filter state */
static float s_dc_prev_in = 0.0f;
static float s_dc_prev_out = 0.0f;
static float s_preemph_prev = 0.0f;

/* Audio task handle */
static TaskHandle_t s_audio_task_handle = NULL;
static volatile bool s_stop_playback = false;

/* Playback data (set before starting playback) */
static const int16_t *s_play_data = NULL;
static size_t s_play_length = 0;
static uint32_t s_play_sample_rate = AUDIO_SAMPLE_RATE_PLAY;

/* Streaming recording state */
static audio_stream_cb_t s_stream_cb = NULL;
static volatile bool s_streaming = false;

/* Streaming playback state (ring buffer) */
static RingbufHandle_t s_play_ringbuf = NULL;
static volatile bool s_stream_playing = false;
static volatile bool s_stream_end_signaled = false;
static volatile bool s_stream_flush_requested = false;
static uint8_t s_output_volume = 90;

/* Silence buffer for underrun padding (prevents clicks/pops) */
static int16_t s_silence_buf[512] = {0};  /* 512 samples = ~21ms at 24kHz */

/* Diagnostics */
static uint32_t s_underrun_count = 0;
static uint32_t s_overrun_count = 0;

/* Streaming pre-roll buffer to avoid clipping first speech phonemes */
static int16_t s_stream_preroll[VAD_STREAM_PREROLL_MAX_CHUNKS][AUDIO_REC_CHUNK_SAMPLES];
static int64_t s_stream_startup_passthrough_until_us = 0;

static uint8_t detect_es8311_addr(i2c_master_bus_handle_t bus_handle)
{
    static const uint16_t candidate_7bit[] = {0x18, 0x19};
    static const uint8_t candidate_8bit[] = {0x30, 0x32};

    for (size_t i = 0; i < sizeof(candidate_7bit) / sizeof(candidate_7bit[0]); i++) {
        if (i2c_master_probe(bus_handle, candidate_7bit[i], 50) == ESP_OK) {
            ESP_LOGI(TAG, "Detected ES8311 at 7-bit 0x%02X (8-bit 0x%02X)",
                     candidate_7bit[i], candidate_8bit[i]);
            return candidate_8bit[i];
        }
    }

    ESP_LOGW(TAG, "Could not probe ES8311 at 0x18/0x19, using configured 8-bit addr 0x%02X",
             ES8311_I2C_ADDR);
    return ES8311_I2C_ADDR;
}

/* ---- VAD ---- */

static uint16_t compute_rms(const int16_t *samples, size_t num_samples)
{
    if (num_samples == 0) return 0;
    int64_t sum_sq = 0;
    for (size_t i = 0; i < num_samples; i++) {
        int32_t s = samples[i];
        sum_sq += s * s;
    }
    return (uint16_t)sqrtf((float)sum_sq / num_samples);
}

/* ---- DSP processing ---- */

static void dsp_process_chunk(int16_t *samples, size_t num_samples)
{
    /*
     * 1. DC offset removal – first-order IIR high-pass filter
     *    y[n] = alpha * (y[n-1] + x[n] - x[n-1])
     *    alpha = 0.995  =>  ~25 Hz cutoff at 16 kHz sample rate
     *    Removes DC bias from the ES8311 ADC.
     */
    const float alpha = 0.995f;
    for (size_t i = 0; i < num_samples; i++) {
        float x = (float)samples[i];
        float y = alpha * (s_dc_prev_out + x - s_dc_prev_in);
        s_dc_prev_in = x;
        s_dc_prev_out = y;
        samples[i] = (int16_t)y;
    }

    /*
     * 2. Pre-emphasis filter – boosts higher frequencies where speech
     *    formants are concentrated, improving STT accuracy.
     *    y[n] = x[n] - coeff * x[n-1]     (coeff = 0.97)
     */
    const float coeff = 0.97f;
    for (size_t i = 0; i < num_samples; i++) {
        float x = (float)samples[i];
        float y = x - coeff * s_preemph_prev;
        s_preemph_prev = x;
        if (y > 32767.0f) y = 32767.0f;
        if (y < -32768.0f) y = -32768.0f;
        samples[i] = (int16_t)y;
    }
}

static void dsp_process_stream_chunk(int16_t *samples, size_t num_samples)
{
    /*
     * Live streaming favors intelligibility over aggressive pre-emphasis.
     * Apply only DC removal to avoid adding hiss/static artifacts.
     */
    const float alpha = 0.995f;
    for (size_t i = 0; i < num_samples; i++) {
        float x = (float)samples[i];
        float y = alpha * (s_dc_prev_out + x - s_dc_prev_in);
        s_dc_prev_in = x;
        s_dc_prev_out = y;
        samples[i] = (int16_t)y;
    }
}

/* ---- I2S setup ---- */

static esp_err_t i2s_init(void)
{
    /* Allocate I2S channels (full duplex) */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_i2s_tx_chan, &s_i2s_rx_chan));

    /* Standard I2S config for recording (16kHz) */
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_SAMPLE_RATE_REC,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_PIN,
            .bclk = I2S_BCLK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DOUT_PIN,
            .din = I2S_DIN_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_rx_chan, &std_cfg));

    return ESP_OK;
}

static esp_err_t i2s_set_sample_rate(uint32_t rate)
{
    i2s_std_clk_config_t clk_cfg = {
        .sample_rate_hz = rate,
        .clk_src = I2S_CLK_SRC_DEFAULT,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
    };
    esp_err_t ret = i2s_channel_reconfig_std_clock(s_i2s_tx_chan, &clk_cfg);
    if (ret != ESP_OK) return ret;
    ret = i2s_channel_reconfig_std_clock(s_i2s_rx_chan, &clk_cfg);
    if (ret != ESP_OK) return ret;

    return ESP_OK;
}

/* ---- ES8311 codec setup via esp_codec_dev ---- */

static esp_err_t codec_init(i2c_master_bus_handle_t bus_handle)
{
    uint8_t es8311_addr = detect_es8311_addr(bus_handle);

    /* Create I2C control interface for ES8311 */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_MASTER_NUM,
        .addr = es8311_addr,
        .bus_handle = bus_handle,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!s_ctrl_if) {
        ESP_LOGE(TAG, "Failed to create I2C control interface");
        return ESP_FAIL;
    }

    /* Create GPIO interface for PA control */
    s_gpio_if = audio_codec_new_gpio();

    /* Create ES8311 codec interface */
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = s_ctrl_if,
        .gpio_if = s_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = PA_ENABLE_PIN,
        .use_mclk = true,
    };
    s_codec_if = es8311_codec_new(&es8311_cfg);
    if (!s_codec_if) {
        ESP_LOGE(TAG, "Failed to create ES8311 codec");
        return ESP_FAIL;
    }

    /* Create I2S data interface */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM,
        .rx_handle = s_i2s_rx_chan,
        .tx_handle = s_i2s_tx_chan,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!s_data_if) {
        ESP_LOGE(TAG, "Failed to create I2S data interface");
        return ESP_FAIL;
    }

    /* Create codec device */
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = s_codec_if,
        .data_if = s_data_if,
    };
    s_codec_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_codec_dev) {
        ESP_LOGE(TAG, "Failed to create codec device");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ES8311 codec initialized");
    return ESP_OK;
}

/* ---- Audio task ---- */

static void audio_record_loop(void)
{
    s_rec_length = 0;
    int silence_ms = 0;
    int speech_ms = 0;
    bool speech_started = false;

    /* Reset DSP filter state for fresh recording */
    s_dc_prev_in = 0.0f;
    s_dc_prev_out = 0.0f;
    s_preemph_prev = 0.0f;

    if (i2s_set_sample_rate(AUDIO_SAMPLE_RATE_REC) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set I2S sample rate for recording");
        s_recording = false;
        return;
    }

    /* Open codec for recording */
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_SAMPLE_RATE_REC,
        .channel = AUDIO_REC_INPUT_CHANNELS,
        .bits_per_sample = AUDIO_BIT_WIDTH,
    };
    if (esp_codec_dev_open(s_codec_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to open codec for recording");
        s_recording = false;
        return;
    }

    ESP_LOGI(TAG, "Recording started, waiting for speech...");

    while (s_recording && s_rec_length < AUDIO_REC_BUF_SIZE) {
        int16_t chunk_stereo[AUDIO_REC_CHUNK_SAMPLES * AUDIO_REC_INPUT_CHANNELS];
        int16_t chunk_mono[AUDIO_REC_CHUNK_SAMPLES];
        size_t stereo_bytes = AUDIO_REC_CHUNK_BYTES * AUDIO_REC_INPUT_CHANNELS;
        esp_err_t ret = esp_codec_dev_read(s_codec_dev, chunk_stereo, stereo_bytes);
        if (ret != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        /* Average both channels into mono (preserves signal from both mics) */
        for (size_t i = 0; i < AUDIO_REC_CHUNK_SAMPLES; i++) {
            int16_t l = chunk_stereo[i * 2];
            int16_t r = chunk_stereo[i * 2 + 1];
            chunk_mono[i] = (int16_t)(((int32_t)l + (int32_t)r) / 2);
        }

        /* Apply DSP: DC offset removal + pre-emphasis */
        dsp_process_chunk(chunk_mono, AUDIO_REC_CHUNK_SAMPLES);

        uint16_t energy = compute_rms(chunk_mono, AUDIO_REC_CHUNK_SAMPLES);
        s_current_energy = energy;

        float chunk_duration_ms = (float)AUDIO_REC_CHUNK_SAMPLES / AUDIO_SAMPLE_RATE_REC * 1000.0f;

        if (energy > VAD_ENERGY_THRESHOLD) {
            silence_ms = 0;
            speech_ms += (int)chunk_duration_ms;

            if (!speech_started && speech_ms >= VAD_MIN_SPEECH_MS) {
                speech_started = true;
                if (s_vad_cb) s_vad_cb(true);
                ESP_LOGI(TAG, "Speech detected");
            }
        } else {
            if (speech_started) {
                silence_ms += (int)chunk_duration_ms;
                if (silence_ms >= VAD_SILENCE_TIMEOUT_MS) {
                    ESP_LOGI(TAG, "Silence detected, stopping recording");
                    break;
                }
            }
        }

        /* Always buffer audio once speech has started */
        if (speech_started) {
            size_t remaining = AUDIO_REC_BUF_SIZE - s_rec_length;
            size_t to_copy = AUDIO_REC_CHUNK_BYTES < remaining ? AUDIO_REC_CHUNK_BYTES : remaining;
            memcpy((uint8_t *)s_rec_buffer + s_rec_length, chunk_mono, to_copy);
            s_rec_length += to_copy;
        }
    }

    esp_codec_dev_close(s_codec_dev);
    s_recording = false;
    s_current_energy = 0;

    if (s_vad_cb) s_vad_cb(false);

    ESP_LOGI(TAG, "Recording done: %u bytes (%.1f sec)",
             (unsigned)s_rec_length,
             (float)s_rec_length / (AUDIO_SAMPLE_RATE_REC * 2));
}

static void audio_playback_loop(void)
{
    if (!s_play_data || s_play_length == 0) {
        s_playing = false;
        return;
    }

    if (i2s_set_sample_rate(s_play_sample_rate) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set I2S sample rate for playback");
        s_playing = false;
        return;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = s_play_sample_rate,
        .channel = AUDIO_CHANNELS,
        .bits_per_sample = AUDIO_BIT_WIDTH,
    };
    if (esp_codec_dev_open(s_codec_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to open codec for playback");
        s_playing = false;
        return;
    }

    /* Enable PA amplifier */
    gpio_set_level(PA_ENABLE_PIN, 1);

    esp_codec_dev_set_out_vol(s_codec_dev, s_output_volume);

    ESP_LOGI(TAG, "Playback started: %u bytes at %lu Hz",
             (unsigned)s_play_length, (unsigned long)s_play_sample_rate);

    size_t offset = 0;
    size_t chunk_size = 1024;

    while (offset < s_play_length && !s_stop_playback) {
        size_t remaining = s_play_length - offset;
        size_t to_write = remaining < chunk_size ? remaining : chunk_size;

        /* Compute energy for sound bar visualization */
        size_t num_samples = to_write / sizeof(int16_t);
        s_current_energy = compute_rms(s_play_data + (offset / sizeof(int16_t)), num_samples);

        esp_codec_dev_write(s_codec_dev, (void *)(s_play_data + (offset / sizeof(int16_t))), to_write);
        offset += to_write;
    }

    gpio_set_level(PA_ENABLE_PIN, 0);
    esp_codec_dev_close(s_codec_dev);

    s_playing = false;
    s_stop_playback = false;
    s_current_energy = 0;

    ESP_LOGI(TAG, "Playback done");
}

/* ---- Streaming recording loop ---- */

static void audio_stream_record_loop(void)
{
    const uint32_t stream_rate = AUDIO_SAMPLE_RATE_REC;
    const int chunk_ms = (int)((AUDIO_REC_CHUNK_SAMPLES * 1000) / stream_rate);
    bool voice_active = false;
    int speech_ms = 0;
    int silence_ms = 0;
    int reopen_block_ms = 0;
    uint32_t noise_floor = VAD_ENERGY_THRESHOLD;
    int min_speech_ms = VAD_STREAM_MIN_SPEECH_MS;
    if (min_speech_ms < chunk_ms) {
        min_speech_ms = chunk_ms;
    }
    int preroll_target_chunks = 0;
    if (VAD_STREAM_PREROLL_MS > 0) {
        preroll_target_chunks = (VAD_STREAM_PREROLL_MS + chunk_ms - 1) / chunk_ms;
        if (preroll_target_chunks > VAD_STREAM_PREROLL_MAX_CHUNKS) {
            preroll_target_chunks = VAD_STREAM_PREROLL_MAX_CHUNKS;
        }
        if (preroll_target_chunks < 1) {
            preroll_target_chunks = 1;
        }
    }
    int preroll_head = 0;
    int preroll_count = 0;
    bool startup_passthrough_consumed = false;

    /* Reset DSP filter state */
    s_dc_prev_in = 0.0f;
    s_dc_prev_out = 0.0f;
    s_preemph_prev = 0.0f;

    if (i2s_set_sample_rate(stream_rate) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set I2S sample rate for streaming");
        s_streaming = false;
        return;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = stream_rate,
        .channel = AUDIO_REC_INPUT_CHANNELS,
        .bits_per_sample = AUDIO_BIT_WIDTH,
    };
    if (esp_codec_dev_open(s_codec_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to open codec for streaming recording");
        s_streaming = false;
        return;
    }

    ESP_LOGI(TAG, "Streaming recording started");

    while (s_streaming) {
        int16_t chunk_stereo[AUDIO_REC_CHUNK_SAMPLES * AUDIO_REC_INPUT_CHANNELS];
        int16_t chunk_mono[AUDIO_REC_CHUNK_SAMPLES];
        size_t stereo_bytes = AUDIO_REC_CHUNK_BYTES * AUDIO_REC_INPUT_CHANNELS;
        esp_err_t ret = esp_codec_dev_read(s_codec_dev, chunk_stereo, stereo_bytes);
        if (ret != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        /* Downmix multi-channel capture to mono for BLE uplink. */
        for (size_t i = 0; i < AUDIO_REC_CHUNK_SAMPLES; i++) {
#if AUDIO_REC_INPUT_CHANNELS >= 2
            int16_t l = chunk_stereo[i * AUDIO_REC_INPUT_CHANNELS];
            int16_t r = chunk_stereo[i * AUDIO_REC_INPUT_CHANNELS + 1];
            chunk_mono[i] = (int16_t)(((int32_t)l + (int32_t)r) / 2);
#else
            chunk_mono[i] = chunk_stereo[i];
#endif
        }

        /* Streaming DSP: DC removal only (avoid pre-emphasis hiss). */
        dsp_process_stream_chunk(chunk_mono, AUDIO_REC_CHUNK_SAMPLES);

        /* Update energy for UI */
        uint16_t energy = compute_rms(chunk_mono, AUDIO_REC_CHUNK_SAMPLES);
        s_current_energy = energy;
        bool startup_passthrough_active =
            (esp_timer_get_time() < s_stream_startup_passthrough_until_us);
        if (startup_passthrough_active) {
            startup_passthrough_consumed = true;
        }

        if (!voice_active && preroll_target_chunks > 0) {
            memcpy(s_stream_preroll[preroll_head], chunk_mono, AUDIO_REC_CHUNK_BYTES);
            preroll_head++;
            if (preroll_head >= preroll_target_chunks) {
                preroll_head = 0;
            }
            if (preroll_count < preroll_target_chunks) {
                preroll_count++;
            }
        }

        /*
         * Adaptive VAD:
         * - Track ambient noise floor.
         * - Require stable speech for VAD_MIN_SPEECH_MS before opening.
         * - Use hysteresis + silence debounce for closing.
         */
        if (energy <= noise_floor) {
            noise_floor = (uint32_t)(((noise_floor * 31U) + energy) / 32U);
        } else {
            noise_floor = (uint32_t)(((noise_floor * 63U) + energy) / 64U);
        }

        uint32_t start_threshold = VAD_ENERGY_THRESHOLD;
        uint32_t adaptive_start = noise_floor + VAD_STREAM_START_MARGIN;
        uint32_t ratio_start = (noise_floor * 16U) / 10U;
        if (adaptive_start > start_threshold) {
            start_threshold = adaptive_start;
        }
        if (ratio_start > start_threshold) {
            start_threshold = ratio_start;
        }
        if (start_threshold < (uint32_t)(VAD_ENERGY_THRESHOLD + VAD_STREAM_START_FLOOR_MARGIN)) {
            start_threshold = (uint32_t)(VAD_ENERGY_THRESHOLD + VAD_STREAM_START_FLOOR_MARGIN);
        }
        if (start_threshold > VAD_STREAM_START_THRESHOLD_MAX) {
            start_threshold = VAD_STREAM_START_THRESHOLD_MAX;
        }
        uint32_t stop_threshold = noise_floor + VAD_STREAM_STOP_MARGIN;
        if (stop_threshold > start_threshold) {
            stop_threshold = start_threshold;
        }

        bool opened_this_chunk = false;
        bool suppress_preroll_on_open = false;

        if (!voice_active) {
            if (reopen_block_ms > 0) {
                reopen_block_ms -= chunk_ms;
                if (reopen_block_ms < 0) {
                    reopen_block_ms = 0;
                }
                speech_ms = 0;
                if (!startup_passthrough_active) {
                    continue;
                }
            }
            if (energy >= start_threshold) {
                speech_ms += chunk_ms;
                if (speech_ms >= min_speech_ms) {
                    voice_active = true;
                    opened_this_chunk = true;
                    suppress_preroll_on_open = startup_passthrough_consumed;
                    startup_passthrough_consumed = false;
                    speech_ms = 0;
                    silence_ms = 0;
                    if (s_vad_cb) {
                        s_vad_cb(true);
                    }
                    ESP_LOGI(TAG,
                             "Stream VAD open: energy=%u start_th=%u noise=%u preroll=%d",
                             (unsigned)energy,
                             (unsigned)start_threshold,
                             (unsigned)noise_floor,
                             preroll_count);
                }
            } else {
                speech_ms = 0;
            }
        } else {
            if (energy >= stop_threshold) {
                silence_ms = 0;
            } else {
                silence_ms += chunk_ms;
                if (silence_ms >= VAD_LIVE_SILENCE_DEBOUNCE_MS) {
                    voice_active = false;
                    speech_ms = 0;
                    silence_ms = 0;
                    reopen_block_ms = VAD_STREAM_REOPEN_BLOCK_MS;
                    if (s_vad_cb) {
                        s_vad_cb(false);
                    }
                    ESP_LOGI(TAG,
                             "Stream VAD close: energy=%u stop_th=%u noise=%u reopen_ms=%d",
                             (unsigned)energy,
                             (unsigned)stop_threshold,
                             (unsigned)noise_floor,
                             reopen_block_ms);
                }
            }
        }

        /* Deliver chunk to callback while VAD-open or in startup passthrough window. */
        if (s_stream_cb && (voice_active || startup_passthrough_active)) {
            bool sent_current_in_preroll = false;
            if (voice_active &&
                opened_this_chunk &&
                preroll_count > 0 &&
                preroll_target_chunks > 0 &&
                !suppress_preroll_on_open &&
                !startup_passthrough_active) {
                int start = preroll_head - preroll_count;
                while (start < 0) {
                    start += preroll_target_chunks;
                }
                for (int i = 0; i < preroll_count; i++) {
                    int idx = start + i;
                    if (idx >= preroll_target_chunks) {
                        idx -= preroll_target_chunks;
                    }
                    s_stream_cb(s_stream_preroll[idx], AUDIO_REC_CHUNK_BYTES);
                }
                sent_current_in_preroll = true;
                preroll_count = 0;
            } else if (voice_active && opened_this_chunk && suppress_preroll_on_open) {
                preroll_count = 0;
            }
            if (!sent_current_in_preroll) {
                s_stream_cb(chunk_mono, AUDIO_REC_CHUNK_BYTES);
            }
        }
    }

    esp_codec_dev_close(s_codec_dev);
    if (voice_active && s_vad_cb) {
        s_vad_cb(false);
    }
    s_current_energy = 0;
    ESP_LOGI(TAG, "Streaming recording stopped");
}

/* ---- Streaming playback loop (ring buffer) ---- */

static void audio_stream_playback_loop(void)
{
    if (!s_play_ringbuf) {
        s_stream_playing = false;
        return;
    }

    if (i2s_set_sample_rate(s_play_sample_rate) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set I2S sample rate for stream playback");
        s_stream_playing = false;
        return;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = s_play_sample_rate,
        .channel = AUDIO_CHANNELS,
        .bits_per_sample = AUDIO_BIT_WIDTH,
    };
    if (esp_codec_dev_open(s_codec_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to open codec for stream playback");
        s_stream_playing = false;
        return;
    }

    gpio_set_level(PA_ENABLE_PIN, 1);
    esp_codec_dev_set_out_vol(s_codec_dev, s_output_volume);

    /* Reset diagnostics */
    s_underrun_count = 0;
    s_overrun_count = 0;

    ESP_LOGI(TAG, "Stream playback started at %lu Hz", (unsigned long)s_play_sample_rate);

    /*
     * Warm up playback buffer to smooth first words.
     * Without this, startup jitter can cause audible stutter/underruns.
     */
    {
        const size_t start_threshold = 24576; /* ~512ms at 24kHz mono 16-bit */
        int waited_ms = 0;
        while (s_stream_playing && !s_stream_flush_requested && !s_stream_end_signaled) {
            size_t free_bytes = xRingbufferGetCurFreeSize(s_play_ringbuf);
            size_t filled_bytes = (free_bytes <= AUDIO_PLAY_RINGBUF_SIZE)
                                  ? (AUDIO_PLAY_RINGBUF_SIZE - free_bytes)
                                  : 0;
            if (filled_bytes >= start_threshold || waited_ms >= 1200) {
                if (waited_ms > 0) {
                    ESP_LOGI(TAG, "Playback prebuffer: filled=%u bytes after %d ms",
                             (unsigned)filled_bytes, waited_ms);
                }
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            waited_ms += 10;
        }
    }

    /* 20ms chunks match app-side pacing and reduce jitter spikes. */
    const size_t PLAYBACK_CHUNK = 960;

    /*
     * Safety timeout: if the ring buffer stays empty for too long without an
     * explicit stream-end signal, break out to prevent the device from hanging
     * permanently in the RESPONDING state (e.g. due to a missed "ready" command).
     */
    int64_t underrun_start_us = 0;
    const int64_t UNDERRUN_TIMEOUT_US = 5000000; /* 5 seconds */

    while (s_stream_playing && !s_stream_flush_requested) {
        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(s_play_ringbuf, &item_size,
                                             pdMS_TO_TICKS(30), PLAYBACK_CHUNK);

        if (item && item_size > 0) {
            underrun_start_us = 0;

            /* Compute energy for visualization */
            size_t num_samples = item_size / sizeof(int16_t);
            s_current_energy = compute_rms((const int16_t *)item, num_samples);

            esp_codec_dev_write(s_codec_dev, item, item_size);
            vRingbufferReturnItem(s_play_ringbuf, item);
        } else if (s_stream_end_signaled) {
            /* No more data and end was signaled */
            break;
        } else {
            /*
             * Buffer underrun: write silence to keep the audio pipeline
             * flowing and avoid clicks/pops from the codec going idle.
             */
            s_underrun_count++;
            if ((s_underrun_count % 20) == 1) {
                ESP_LOGW(TAG, "Audio underrun #%lu (ring buffer empty)",
                         (unsigned long)s_underrun_count);
            }

            if (underrun_start_us == 0) {
                underrun_start_us = esp_timer_get_time();
            } else if ((esp_timer_get_time() - underrun_start_us) >= UNDERRUN_TIMEOUT_US) {
                ESP_LOGW(TAG, "Stream starved for >3s with no end signal, auto-ending");
                break;
            }

            /* Play a small silence block to keep codec active */
            esp_codec_dev_write(s_codec_dev, s_silence_buf, sizeof(s_silence_buf));
            s_current_energy = 0;
        }
    }

    gpio_set_level(PA_ENABLE_PIN, 0);
    esp_codec_dev_close(s_codec_dev);

    if (s_underrun_count > 0 || s_overrun_count > 0) {
        ESP_LOGW(TAG, "Stream playback stats: %lu underruns, %lu overruns",
                 (unsigned long)s_underrun_count, (unsigned long)s_overrun_count);
    }

    s_stream_playing = false;
    s_stream_end_signaled = false;
    s_stream_flush_requested = false;
    s_current_energy = 0;

    ESP_LOGI(TAG, "Stream playback done");
}

static void audio_task(void *arg)
{
    extern EventGroupHandle_t g_app_events;

    while (1) {
        /* Wait for recording, playback, or streaming commands */
        EventBits_t bits = xEventGroupWaitBits(
            g_app_events,
            EVT_START_RECORDING | EVT_STOP_PLAYBACK | EVT_START_STREAMING | EVT_STOP_STREAMING,
            pdTRUE, pdFALSE,
            pdMS_TO_TICKS(100)
        );

        if (bits & EVT_STOP_STREAMING) {
            s_streaming = false;
        }

        if (bits & EVT_START_STREAMING) {
            s_streaming = true;
            audio_stream_record_loop();
        }

        if (bits & EVT_START_RECORDING) {
            s_recording = true;
            audio_record_loop();
            xEventGroupSetBits(g_app_events, EVT_RECORDING_DONE);
        }

        if (bits & EVT_STOP_PLAYBACK) {
            s_stop_playback = true;
        }

        /* Check if batch playback was requested */
        if (s_playing && !s_stop_playback) {
            audio_playback_loop();
            xEventGroupSetBits(g_app_events, EVT_PLAYBACK_DONE);
        }

        if (s_stream_playing) {
            audio_stream_playback_loop();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ---- Public API ---- */

esp_err_t audio_hal_init(i2c_master_bus_handle_t bus_handle)
{
    ESP_LOGI(TAG, "Initializing audio subsystem...");

    /* Allocate recording buffer in PSRAM */
    s_rec_buffer = heap_caps_malloc(AUDIO_REC_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_rec_buffer) {
        ESP_LOGE(TAG, "Failed to allocate recording buffer in PSRAM");
        return ESP_ERR_NO_MEM;
    }

    /* Configure PA enable pin */
    gpio_config_t pa_cfg = {
        .pin_bit_mask = (1ULL << PA_ENABLE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_cfg);
    gpio_set_level(PA_ENABLE_PIN, 0);

    /* Initialize I2S */
    esp_err_t ret = i2s_init();
    if (ret != ESP_OK) return ret;

    /* Initialize ES8311 codec */
    ret = codec_init(bus_handle);
    if (ret != ESP_OK) return ret;

    /* Increase microphone sensitivity and make sure input is unmuted */
    esp_codec_dev_set_in_mute(s_codec_dev, false);
    esp_codec_dev_set_in_gain(s_codec_dev, AUDIO_INPUT_GAIN_DB);

    /* Create audio task on Core 1 */
    xTaskCreatePinnedToCore(
        audio_task, "audio_task",
        AUDIO_TASK_STACK_SIZE, NULL,
        AUDIO_TASK_PRIORITY, &s_audio_task_handle,
        AUDIO_TASK_CORE
    );

    ESP_LOGI(TAG, "Audio subsystem initialized");
    return ESP_OK;
}

esp_err_t audio_hal_start_recording(void)
{
    if (s_recording || s_playing) return ESP_ERR_INVALID_STATE;
    extern EventGroupHandle_t g_app_events;
    xEventGroupSetBits(g_app_events, EVT_START_RECORDING);
    return ESP_OK;
}

esp_err_t audio_hal_stop_recording(void)
{
    s_recording = false;
    return ESP_OK;
}

esp_err_t audio_hal_get_recording(const int16_t **data, size_t *length_bytes)
{
    if (!data || !length_bytes) return ESP_ERR_INVALID_ARG;
    *data = s_rec_buffer;
    *length_bytes = s_rec_length;
    return ESP_OK;
}

esp_err_t audio_hal_play_audio(const int16_t *data, size_t length_bytes, uint32_t sample_rate)
{
    if (s_playing || s_recording) return ESP_ERR_INVALID_STATE;
    if (!data || length_bytes == 0) return ESP_ERR_INVALID_ARG;

    s_play_data = data;
    s_play_length = length_bytes;
    s_play_sample_rate = sample_rate;
    s_stop_playback = false;
    s_playing = true;

    /* The audio_task will pick this up and start playback */
    return ESP_OK;
}

esp_err_t audio_hal_stop_playback(void)
{
    extern EventGroupHandle_t g_app_events;
    xEventGroupSetBits(g_app_events, EVT_STOP_PLAYBACK);
    return ESP_OK;
}

bool audio_hal_is_playing(void)
{
    return s_playing;
}

bool audio_hal_is_recording(void)
{
    return s_recording;
}

esp_err_t audio_hal_set_volume(uint8_t volume)
{
    if (!s_codec_dev) return ESP_ERR_INVALID_STATE;
    if (volume > 100) volume = 100;
    s_output_volume = volume;
    return esp_codec_dev_set_out_vol(s_codec_dev, s_output_volume);
}

void audio_hal_register_vad_cb(vad_callback_t cb)
{
    s_vad_cb = cb;
}

uint16_t audio_hal_get_current_energy(void)
{
    return s_current_energy;
}

/* ---- Streaming recording API ---- */

esp_err_t audio_hal_start_streaming(audio_stream_cb_t cb)
{
    if (s_recording || s_playing || s_streaming) return ESP_ERR_INVALID_STATE;
    if (!cb) return ESP_ERR_INVALID_ARG;

    s_stream_cb = cb;
    if (MIC_STARTUP_PASSTHROUGH_MS > 0) {
        s_stream_startup_passthrough_until_us =
            esp_timer_get_time() + ((int64_t)MIC_STARTUP_PASSTHROUGH_MS * 1000);
    } else {
        s_stream_startup_passthrough_until_us = 0;
    }
    extern EventGroupHandle_t g_app_events;
    xEventGroupSetBits(g_app_events, EVT_START_STREAMING);
    return ESP_OK;
}

esp_err_t audio_hal_stop_streaming(void)
{
    s_streaming = false;
    s_stream_startup_passthrough_until_us = 0;
    extern EventGroupHandle_t g_app_events;
    xEventGroupSetBits(g_app_events, EVT_STOP_STREAMING);
    return ESP_OK;
}

bool audio_hal_is_streaming(void)
{
    return s_streaming;
}

/* ---- Streaming playback API (ring buffer) ---- */

esp_err_t audio_hal_start_stream_playback(uint32_t sample_rate)
{
    if (s_stream_playing || s_playing) return ESP_ERR_INVALID_STATE;

    /* Create ring buffer in PSRAM */
    if (!s_play_ringbuf) {
        s_play_ringbuf = xRingbufferCreateWithCaps(AUDIO_PLAY_RINGBUF_SIZE,
                                                    RINGBUF_TYPE_BYTEBUF,
                                                    MALLOC_CAP_SPIRAM);
        if (!s_play_ringbuf) {
            ESP_LOGE(TAG, "Failed to create playback ring buffer");
            return ESP_ERR_NO_MEM;
        }
    }

    s_play_sample_rate = sample_rate;
    s_stream_end_signaled = false;
    s_stream_flush_requested = false;
    s_stream_playing = true;

    return ESP_OK;
}

esp_err_t audio_hal_stream_write(const int16_t *data, size_t bytes)
{
    if (!s_play_ringbuf || !s_stream_playing) return ESP_ERR_INVALID_STATE;
    if (!data || bytes == 0 || (bytes & 0x1) != 0) return ESP_ERR_INVALID_ARG;

    BaseType_t ret = xRingbufferSend(s_play_ringbuf, data, bytes, pdMS_TO_TICKS(5));
    if (ret != pdTRUE) {
        s_overrun_count++;
        if ((s_overrun_count % 10) == 1) {
            ESP_LOGW(TAG, "Ring buffer full (overrun #%lu), dropping %u bytes",
                     (unsigned long)s_overrun_count, (unsigned)bytes);
        }
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t audio_hal_stream_end(void)
{
    s_stream_end_signaled = true;
    return ESP_OK;
}

esp_err_t audio_hal_stream_flush(void)
{
    /* Signal flush (barge-in) - stop playback immediately */
    s_stream_flush_requested = true;

    /* Wait for playback to stop */
    int timeout = 50;
    while (s_stream_playing && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* Drain ring buffer */
    if (s_play_ringbuf) {
        size_t item_size;
        void *item;
        while ((item = xRingbufferReceive(s_play_ringbuf, &item_size, 0)) != NULL) {
            vRingbufferReturnItem(s_play_ringbuf, item);
        }
    }

    return ESP_OK;
}

bool audio_hal_is_stream_playing(void)
{
    return s_stream_playing;
}
