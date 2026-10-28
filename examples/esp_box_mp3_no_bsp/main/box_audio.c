/*
 * SPDX-FileCopyrightText: 2026 Chris Morgan
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stddef.h>

#include "box_audio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

static const char *TAG = "box_audio";

esp_err_t box_audio_init(esp_codec_dev_handle_t *speaker)
{
    ESP_RETURN_ON_FALSE(speaker != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "speaker output handle is required");
    *speaker = NULL;

    /* ESP_GOTO_* assigns ret and jumps to cleanup on failure. The host
     * analysis stubs do not define these ESP-IDF macros. */
    // cppcheck-suppress unreadVariable
    esp_err_t ret = ESP_OK;
    i2c_master_bus_handle_t bus = NULL;
    i2s_chan_handle_t tx = NULL;
    const audio_codec_ctrl_if_t *control = NULL;
    const audio_codec_data_if_t *data = NULL;
    const audio_codec_gpio_if_t *gpio = NULL;
    const audio_codec_if_t *codec = NULL;
    esp_codec_dev_handle_t device = NULL;

    /* Control bus and speaker wiring for the original ESP32-S3-BOX. */
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = GPIO_NUM_8,
        .scl_io_num = GPIO_NUM_18,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_GOTO_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), cleanup, TAG,
                      "failed to create I2C master bus");

    i2s_chan_config_t channel_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    channel_cfg.auto_clear_after_cb = true;
    ESP_GOTO_ON_ERROR(i2s_new_channel(&channel_cfg, &tx, NULL), cleanup, TAG,
                      "failed to create I2S TX channel");

    const i2s_std_config_t output_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_2,
            .bclk = GPIO_NUM_17,
            .ws = GPIO_NUM_47,
            .dout = GPIO_NUM_15,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(tx, &output_cfg), cleanup, TAG,
                      "failed to configure I2S output");
    /* Supply MCLK while the ES8311 driver initializes the codec. */
    ESP_GOTO_ON_ERROR(i2s_channel_enable(tx), cleanup, TAG,
                      "failed to enable I2S output");

    audio_codec_i2c_cfg_t control_cfg = {
        .port = I2C_NUM_1,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
        .clock_speed_hz = 100000,
    };
    control = audio_codec_new_i2c_ctrl(&control_cfg);
    ESP_GOTO_ON_FALSE(control != NULL, ESP_FAIL, cleanup, TAG,
                      "failed to create codec control interface");

    audio_codec_i2s_cfg_t data_cfg = {
        .port = I2S_NUM_1,
        .tx_handle = tx,
    };
    data = audio_codec_new_i2s_data(&data_cfg);
    ESP_GOTO_ON_FALSE(data != NULL, ESP_ERR_NO_MEM, cleanup, TAG,
                      "failed to create codec data interface");

    gpio = audio_codec_new_gpio();
    ESP_GOTO_ON_FALSE(gpio != NULL, ESP_ERR_NO_MEM, cleanup, TAG,
                      "failed to create codec GPIO interface");

    es8311_codec_cfg_t codec_cfg = {
        .ctrl_if = control,
        .gpio_if = gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_46,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .mclk_div = 256,
        .hw_gain = {
            .pa_voltage = 5.0f,
            .codec_dac_voltage = 3.3f,
        },
    };
    codec = es8311_codec_new(&codec_cfg);
    ESP_GOTO_ON_FALSE(codec != NULL, ESP_FAIL, cleanup, TAG,
                      "failed to initialize ES8311 codec");

    esp_codec_dev_cfg_t device_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec,
        .data_if = data,
    };
    device = esp_codec_dev_new(&device_cfg);
    ESP_GOTO_ON_FALSE(device != NULL, ESP_ERR_NO_MEM, cleanup, TAG,
                      "failed to create speaker device");

    esp_codec_dev_sample_info_t format = {
        .sample_rate = 44100,
        .channel = 2,
        .bits_per_sample = 16,
    };
    ESP_GOTO_ON_ERROR(esp_codec_dev_open(device, &format), cleanup, TAG,
                      "failed to open speaker device");
    ESP_GOTO_ON_ERROR(esp_codec_dev_set_out_mute(device, true), cleanup, TAG,
                      "failed to mute speaker");
    ESP_GOTO_ON_ERROR(esp_codec_dev_set_out_vol(device, BOX_AUDIO_VOLUME_PERCENT),
                      cleanup, TAG, "failed to set speaker volume");

    *speaker = device;
    return ESP_OK;

// cppcheck-suppress unusedLabel
cleanup:
    if (device != NULL) {
        esp_codec_dev_delete(device);
    }
    if (codec != NULL) {
        audio_codec_delete_codec_if(codec);
    }
    if (gpio != NULL) {
        audio_codec_delete_gpio_if(gpio);
    }
    if (data != NULL) {
        audio_codec_delete_data_if(data);
    }
    if (control != NULL) {
        audio_codec_delete_ctrl_if(control);
    }
    if (tx != NULL) {
        i2s_chan_info_t channel_info = {0};
        if (i2s_channel_get_info(tx, &channel_info) == ESP_OK &&
            channel_info.is_enabled) {
            i2s_channel_disable(tx);
        }
        i2s_del_channel(tx);
    }
    if (bus != NULL) {
        i2c_del_master_bus(bus);
    }
    return ret;
}
