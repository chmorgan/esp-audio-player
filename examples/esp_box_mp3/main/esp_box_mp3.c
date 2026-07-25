/*
 * SPDX-FileCopyrightText: 2026 Chris Morgan
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "audio_player.h"
#include "audio_stream_io.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SPEAKER_VOLUME_PERCENT 60
#define INITIAL_SAMPLE_RATE_HZ 44100

static const char *TAG = "esp_box_mp3";
static esp_codec_dev_handle_t speaker_codec;
static SemaphoreHandle_t playback_finished;

extern const uint8_t sample_mp3_start[] asm("_binary_sample_mp3_start");
extern const uint8_t sample_mp3_end[] asm("_binary_sample_mp3_end");

static esp_err_t player_mute(AUDIO_PLAYER_MUTE_SETTING setting)
{
    const bool mute = (setting == AUDIO_PLAYER_MUTE);
    esp_err_t ret = esp_codec_dev_set_out_mute(speaker_codec, mute);

    /* The codec can lose its output level while muting, so restore it when
     * playback resumes, as the original ESP-BOX MP3 demo does. */
    if ((ret == ESP_OK) && !mute) {
        ret = esp_codec_dev_set_out_vol(speaker_codec, SPEAKER_VOLUME_PERCENT);
    }

    return ret;
}

static esp_err_t player_write(void *audio_buffer, size_t len,
                              size_t *bytes_written, uint32_t timeout_ms)
{
    (void)timeout_ms;

    esp_err_t ret = esp_codec_dev_write(speaker_codec, audio_buffer, (int)len);
    *bytes_written = (ret == ESP_OK) ? len : 0;
    return ret;
}

static esp_err_t player_set_clock(uint32_t sample_rate, uint32_t bits_per_sample,
                                  i2s_slot_mode_t slot_mode)
{
    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = sample_rate,
        .channel = (slot_mode == I2S_SLOT_MODE_MONO) ? 1 : 2,
        .bits_per_sample = bits_per_sample,
    };

    esp_err_t ret = esp_codec_dev_close(speaker_codec);
    if (ret != ESP_OK) {
        return ret;
    }

    return esp_codec_dev_open(speaker_codec, &sample_info);
}

static void player_event_callback(audio_player_cb_ctx_t *ctx)
{
    if (ctx->audio_event == AUDIO_PLAYER_CALLBACK_EVENT_IDLE) {
        xSemaphoreGive(playback_finished);
    }
}

static esp_err_t speaker_init(void)
{
    speaker_codec = bsp_audio_codec_speaker_init();
    if (speaker_codec == NULL) {
        ESP_LOGE(TAG, "failed to initialize ESP-BOX speaker codec");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t initial_format = {
        .sample_rate = INITIAL_SAMPLE_RATE_HZ,
        .channel = 2,
        .bits_per_sample = 16,
    };

    esp_err_t ret = esp_codec_dev_open(speaker_codec, &initial_format);
    if (ret != ESP_OK) {
        return ret;
    }

    return esp_codec_dev_set_out_vol(speaker_codec, SPEAKER_VOLUME_PERCENT);
}

void app_main(void)
{
    ESP_ERROR_CHECK(speaker_init());

    playback_finished = xSemaphoreCreateBinary();
    if (playback_finished == NULL) {
        ESP_LOGE(TAG, "failed to create playback completion semaphore");
        abort();
    }

    const audio_player_config_t player_config = {
        .mute_fn = player_mute,
        .clk_set_fn = player_set_clock,
        .write_fn = player_write,
        .priority = configMAX_PRIORITIES - 5,
        .coreID = 1,
    };

    ESP_ERROR_CHECK(audio_player_new(player_config));
    ESP_ERROR_CHECK(audio_player_callback_register(player_event_callback, NULL));

    // Linker-generated symbols delimit the same embedded binary object.
    // cppcheck-suppress subtractPointers
    const size_t sample_size = (size_t)(sample_mp3_end - sample_mp3_start);
    ESP_LOGI(TAG, "looping embedded MP3 (%zu bytes)", sample_size);

    while (true) {
        audio_stream_io_handle_t stream =
            audio_stream_io_from_memory(sample_mp3_start, sample_size, false);
        if (stream == NULL) {
            ESP_LOGE(TAG, "failed to create MP3 memory stream; retrying");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        esp_err_t ret = audio_player_play_io(stream);
        if (ret != ESP_OK) {
            /* Ownership transfers only after the play request is accepted. */
            audio_stream_io_close(stream);
            ESP_LOGE(TAG, "failed to start playback: %s; retrying",
                     esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        xSemaphoreTake(playback_finished, portMAX_DELAY);
    }
}
