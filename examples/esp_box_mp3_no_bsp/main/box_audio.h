/*
 * SPDX-FileCopyrightText: 2026 Chris Morgan
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_codec_dev.h"
#include "esp_err.h"

#define BOX_AUDIO_VOLUME_PERCENT 60

/**
 * Initialize the original ESP32-S3-BOX speaker once at application startup.
 *
 * On success, speaker receives an open, muted codec configured for 44.1 kHz,
 * 16-bit stereo at BOX_AUDIO_VOLUME_PERCENT. The buses and codec interfaces
 * remain allocated for the application's lifetime. On failure, partial
 * initialization is released and speaker is set to NULL.
 */
esp_err_t box_audio_init(esp_codec_dev_handle_t *speaker);
