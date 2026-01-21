#pragma once

#include "audio_player.h"

#ifdef __cplusplus
extern "C" {
#endif

struct audio_stream;
typedef struct audio_stream* audio_stream_handle_t;

#define CHECK_STREAM(s) \
    ESP_RETURN_ON_FALSE(s != NULL, ESP_ERR_INVALID_ARG, "audio_stream", "stream is NULL")

typedef enum {
    AUDIO_STREAM_TYPE_UNKNOWN = 0,
    AUDIO_STREAM_TYPE_DECODER,
    AUDIO_STREAM_TYPE_RAW
} audio_stream_type_t;

typedef struct {
    audio_stream_type_t type;    /*< Type of stream */
    char name[16];               /*< Optional: Name of the stream (e.g. "sfx", "bgm"). Auto-generated if empty. */
    UBaseType_t priority;        /*< FreeRTOS task priority */
    BaseType_t coreID;           /*< ESP32 core ID */
} audio_stream_config_t;

#define DEFAULT_AUDIO_STREAM_CONFIG(_name) {    \
        .type = AUDIO_STREAM_TYPE_DECODER,      \
        .name = _name,                          \
        .priority = tskIDLE_PRIORITY + 1,       \
        .coreID = tskNO_AFFINITY                \
    }

/**
 * Stream API — create/delete logical playback streams and control them.
 * These streams own their decode task and submit PCM to the mixer.
 */

audio_player_state_t audio_stream_get_state(audio_stream_handle_t h);
audio_stream_type_t audio_stream_get_type(audio_stream_handle_t h);

esp_err_t audio_stream_play(audio_stream_handle_t h, FILE *fp);
esp_err_t audio_stream_queue(audio_stream_handle_t h, FILE *fp, bool play_now);
esp_err_t audio_stream_stop(audio_stream_handle_t h);
esp_err_t audio_stream_pause(audio_stream_handle_t h);
esp_err_t audio_stream_resume(audio_stream_handle_t h);

/**
 * Direct write raw PCM data to a stream (For RAW streams).
 * Data format must match the mixer configuration (e.g. 44.1kHz, 16-bit, mono).
 */
esp_err_t audio_stream_write_pcm(audio_stream_handle_t h, void *data, size_t size, uint32_t timeout_ms);
esp_err_t audio_stream_raw_send_event(audio_stream_handle_t h, audio_player_callback_event_t event);

audio_stream_handle_t audio_stream_new(audio_stream_config_t *cfg);
esp_err_t audio_stream_delete(audio_stream_handle_t h);

#ifdef __cplusplus
}
#endif
