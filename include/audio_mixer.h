/**
 * @file audio_mixer.h
 * Mixer interface for esp-audio-player. Provides a global mixer that accepts
 * PCM from multiple sources via FreeRTOS ring buffers and writes mixed PCM to I2S.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "audio_player.h"
#include "../audio_decode_types.h"  // FIXME: leaks out
#include "audio_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    audio_player_mute_fn mute_fn;
    audio_reconfig_std_clock clk_set_fn;
    audio_player_write_fn write_fn;
    UBaseType_t priority; /*< FreeRTOS task priority */
    BaseType_t coreID; /*< ESP32 core ID */

    format i2s_format;
} audio_mixer_config_t;

typedef audio_player_cb_t audio_mixer_cb_t;


uint8_t audio_mixer_stream_count();

/** Lock the mixer's main mutex. Call this before modifying stream state (busy flags, queues). */
void audio_mixer_lock();

/** Unlock the mixer's main mutex. */
void audio_mixer_unlock();

/** Add a stream to the mixer's processing list (Thread safe). */
void audio_mixer_add_stream(audio_stream_handle_t h);

/** Remove a stream from the mixer's processing list (Thread safe). */
void audio_mixer_remove_stream(audio_stream_handle_t h);

/** Query the current mixer output format. Returns zeros if not initialized. */
void audio_mixer_get_output_format(uint32_t *sample_rate, uint32_t *bits_per_sample, uint32_t *channels);

void audio_mixer_callback_register(audio_mixer_cb_t cb);

/** Initialize the mixer with fixed output format and start the mixer task. */
esp_err_t audio_mixer_init(audio_mixer_config_t *cfg);

/** Deinitialize the mixer task. */
void audio_mixer_deinit();

#ifdef __cplusplus
}
#endif
