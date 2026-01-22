#include <stdint.h>
#include "esp_log.h"
#include "esp_check.h"
#include "unity.h"
#include "audio_player.h"
#include "audio_mixer.h"
#include "audio_stream.h"
#include "driver/gpio.h"
#include "test_utils.h"
#include "freertos/semphr.h"

static const char *TAG = "AUDIO MIXER TEST";

#define CONFIG_BSP_I2S_NUM 1

/* Audio Pins (same as in audio_player_test.c) */
#define BSP_I2S_SCLK          (GPIO_NUM_17)
#define BSP_I2S_MCLK          (GPIO_NUM_2)
#define BSP_I2S_LCLK          (GPIO_NUM_47)
#define BSP_I2S_DOUT          (GPIO_NUM_15) 
#define BSP_I2S_DSIN          (GPIO_NUM_16) 
#define BSP_POWER_AMP_IO      (GPIO_NUM_46)

#define BSP_I2S_GPIO_CFG       \
    {                          \
        .mclk = BSP_I2S_MCLK,  \
        .bclk = BSP_I2S_SCLK,  \
        .ws = BSP_I2S_LCLK,    \
        .dout = BSP_I2S_DOUT,  \
        .din = BSP_I2S_DSIN,   \
        .invert_flags = {      \
            .mclk_inv = false, \
            .bclk_inv = false, \
            .ws_inv = false,   \
        },                     \
    }

static i2s_chan_handle_t i2s_tx_chan;
static i2s_chan_handle_t i2s_rx_chan;

static esp_err_t bsp_i2s_write(void * audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    return i2s_channel_write(i2s_tx_chan, (char *)audio_buffer, len, bytes_written, timeout_ms);
}

static esp_err_t bsp_i2s_reconfig_clk(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG((i2s_data_bit_width_t)bits_cfg, (i2s_slot_mode_t)ch),
        .gpio_cfg = BSP_I2S_GPIO_CFG,
    };

    i2s_channel_disable(i2s_tx_chan);
    i2s_channel_reconfig_std_clock(i2s_tx_chan, &std_cfg.clk_cfg);
    i2s_channel_reconfig_std_slot(i2s_tx_chan, &std_cfg.slot_cfg);
    return i2s_channel_enable(i2s_tx_chan);
}

static esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx_chan, &i2s_rx_chan));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx_chan, i2s_config));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx_chan));
    return ESP_OK;
}

static void bsp_audio_deinit()
{
    i2s_channel_disable(i2s_tx_chan);
    i2s_del_channel(i2s_tx_chan);
    i2s_del_channel(i2s_rx_chan);
}

TEST_CASE("audio mixer can be initialized and deinitialized", "[audio mixer]")
{
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = BSP_I2S_GPIO_CFG,
    };
    TEST_ESP_OK(bsp_audio_init(&std_cfg));

    audio_mixer_config_t mixer_cfg = {
        .write_fn = bsp_i2s_write,
        .clk_set_fn = bsp_i2s_reconfig_clk,
        .priority = 5,
        .coreID = 0,
        .i2s_format = {
            .sample_rate = 44100,
            .bits_per_sample = 16,
            .channels = 2
        }
    };

    TEST_ESP_OK(audio_mixer_init(&mixer_cfg));
    TEST_ASSERT_TRUE(audio_mixer_is_initialized());

    audio_mixer_deinit();
    TEST_ASSERT_FALSE(audio_mixer_is_initialized());

    bsp_audio_deinit();
}

TEST_CASE("audio streams can be created and deleted", "[audio mixer]")
{
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = BSP_I2S_GPIO_CFG,
    };
    TEST_ESP_OK(bsp_audio_init(&std_cfg));

    audio_mixer_config_t mixer_cfg = {
        .write_fn = bsp_i2s_write,
        .clk_set_fn = bsp_i2s_reconfig_clk,
        .priority = 5,
        .coreID = 0,
        .i2s_format = {
            .sample_rate = 44100,
            .bits_per_sample = 16,
            .channels = 2
        }
    };
    TEST_ESP_OK(audio_mixer_init(&mixer_cfg));

    // Create a decoder stream
    audio_stream_config_t stream_cfg = DEFAULT_AUDIO_STREAM_CONFIG("decoder");
    audio_stream_handle_t decoder_stream = audio_stream_new(&stream_cfg);
    TEST_ASSERT_NOT_NULL(decoder_stream);
    TEST_ASSERT_EQUAL(AUDIO_STREAM_TYPE_DECODER, audio_stream_get_type(decoder_stream));
    TEST_ASSERT_EQUAL(1, audio_mixer_stream_count());

    // Create a raw stream
    audio_stream_config_t raw_cfg = {
        .type = AUDIO_STREAM_TYPE_RAW,
        .name = "raw",
        .priority = 5,
        .coreID = 0
    };
    audio_stream_handle_t raw_stream = audio_stream_new(&raw_cfg);
    TEST_ASSERT_NOT_NULL(raw_stream);
    TEST_ASSERT_EQUAL(AUDIO_STREAM_TYPE_RAW, audio_stream_get_type(raw_stream));
    TEST_ASSERT_EQUAL(2, audio_mixer_stream_count());

    // Delete streams
    TEST_ESP_OK(audio_stream_delete(decoder_stream));
    TEST_ASSERT_EQUAL(1, audio_mixer_stream_count());

    TEST_ESP_OK(audio_stream_delete(raw_stream));
    TEST_ASSERT_EQUAL(0, audio_mixer_stream_count());

    audio_mixer_deinit();
    bsp_audio_deinit();
}

TEST_CASE("audio mixer handles multiple streams and output format", "[audio mixer]")
{
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = BSP_I2S_GPIO_CFG,
    };
    TEST_ESP_OK(bsp_audio_init(&std_cfg));

    audio_mixer_config_t mixer_cfg = {
        .write_fn = bsp_i2s_write,
        .clk_set_fn = bsp_i2s_reconfig_clk,
        .priority = 5,
        .coreID = 0,
        .i2s_format = {
            .sample_rate = 48000,
            .bits_per_sample = 16,
            .channels = 2
        }
    };
    TEST_ESP_OK(audio_mixer_init(&mixer_cfg));

    uint32_t rate, bits, ch;
    audio_mixer_get_output_format(&rate, &bits, &ch);
    TEST_ASSERT_EQUAL(48000, rate);
    TEST_ASSERT_EQUAL(16, bits);
    TEST_ASSERT_EQUAL(2, ch);

    audio_stream_config_t s1_cfg = DEFAULT_AUDIO_STREAM_CONFIG("s1");
    audio_stream_handle_t s1 = audio_stream_new(&s1_cfg);
    (void)s1;
    audio_stream_config_t s2_cfg = DEFAULT_AUDIO_STREAM_CONFIG("s2");
    audio_stream_handle_t s2 = audio_stream_new(&s2_cfg);
    (void)s2;

    TEST_ASSERT_EQUAL(2, audio_mixer_stream_count());

    audio_mixer_deinit(); // Should also clean up streams
    TEST_ASSERT_EQUAL(0, audio_mixer_stream_count());

    bsp_audio_deinit();
}

TEST_CASE("audio stream raw can send events", "[audio mixer]")
{
    audio_stream_config_t raw_cfg = {
        .type = AUDIO_STREAM_TYPE_RAW,
        .name = "raw_event",
        .priority = 5,
        .coreID = 0
    };
    audio_stream_handle_t raw_stream = audio_stream_new(&raw_cfg);
    TEST_ASSERT_NOT_NULL(raw_stream);

    TEST_ASSERT_EQUAL(AUDIO_PLAYER_STATE_IDLE, audio_stream_get_state(raw_stream));

    TEST_ESP_OK(audio_stream_raw_send_event(raw_stream, AUDIO_PLAYER_CALLBACK_EVENT_PLAYING));
    TEST_ASSERT_EQUAL(AUDIO_PLAYER_STATE_PLAYING, audio_stream_get_state(raw_stream));

    TEST_ESP_OK(audio_stream_raw_send_event(raw_stream, AUDIO_PLAYER_CALLBACK_EVENT_IDLE));
    TEST_ASSERT_EQUAL(AUDIO_PLAYER_STATE_IDLE, audio_stream_get_state(raw_stream));

    TEST_ESP_OK(audio_stream_delete(raw_stream));
}

static QueueHandle_t mixer_event_queue;

static void mixer_callback(audio_player_cb_ctx_t *ctx)
{
    if (ctx->audio_event == AUDIO_PLAYER_CALLBACK_EVENT_PLAYING || 
        ctx->audio_event == AUDIO_PLAYER_CALLBACK_EVENT_IDLE) {
        xQueueSend(mixer_event_queue, &(ctx->audio_event), 0);
    }
}

TEST_CASE("audio mixer plays sample mp3 on multiple streams", "[audio mixer]")
{
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = BSP_I2S_GPIO_CFG,
    };
    TEST_ESP_OK(bsp_audio_init(&std_cfg));

    audio_mixer_config_t mixer_cfg = {
        .write_fn = bsp_i2s_write,
        .clk_set_fn = bsp_i2s_reconfig_clk,
        .priority = 5,
        .coreID = 0,
        .i2s_format = {
            .sample_rate = 44100,
            .bits_per_sample = 16,
            .channels = 2
        }
    };
    TEST_ESP_OK(audio_mixer_init(&mixer_cfg));

    mixer_event_queue = xQueueCreate(10, sizeof(audio_player_callback_event_t));
    TEST_ASSERT_NOT_NULL(mixer_event_queue);
    audio_mixer_callback_register(mixer_callback);

    extern const char mp3_start[] asm("_binary_gs_16b_1c_44100hz_mp3_start");
    extern const char mp3_end[]   asm("_binary_gs_16b_1c_44100hz_mp3_end");
    size_t mp3_size = (size_t)((uintptr_t)mp3_end - (uintptr_t)mp3_start);

    // Create two streams
    audio_stream_config_t s1_cfg = DEFAULT_AUDIO_STREAM_CONFIG("stream1");
    audio_stream_handle_t s1 = audio_stream_new(&s1_cfg);
    TEST_ASSERT_NOT_NULL(s1);

    audio_stream_config_t s2_cfg = DEFAULT_AUDIO_STREAM_CONFIG("stream2");
    audio_stream_handle_t s2 = audio_stream_new(&s2_cfg);
    TEST_ASSERT_NOT_NULL(s2);

    // Play on stream 1
    FILE *f1 = fmemopen((void*)mp3_start, mp3_size, "rb");
    TEST_ASSERT_NOT_NULL(f1);
    TEST_ESP_OK(audio_stream_play(s1, f1));

    // Play on stream 2
    FILE *f2 = fmemopen((void*)mp3_start, mp3_size, "rb");
    TEST_ASSERT_NOT_NULL(f2);
    TEST_ESP_OK(audio_stream_play(s2, f2));

    audio_player_callback_event_t event;
    // We expect two PLAYING events (one for each stream)
    int playing_count = 0;
    while (playing_count < 2 && xQueueReceive(mixer_event_queue, &event, pdMS_TO_TICKS(500)) == pdPASS) {
        if (event == AUDIO_PLAYER_CALLBACK_EVENT_PLAYING) {
            playing_count++;
        }
    }
    TEST_ASSERT_EQUAL(2, playing_count);

    // Let it play for a few seconds
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Stop streams
    TEST_ESP_OK(audio_stream_stop(s1));
    TEST_ESP_OK(audio_stream_stop(s2));

    audio_mixer_deinit();
    vQueueDelete(mixer_event_queue);
    bsp_audio_deinit();
}

TEST_CASE("audio stream pause and resume", "[audio mixer]")
{
    audio_stream_config_t stream_cfg = DEFAULT_AUDIO_STREAM_CONFIG("pause_resume");
    audio_stream_handle_t s = audio_stream_new(&stream_cfg);
    TEST_ASSERT_NOT_NULL(s);

    TEST_ESP_OK(audio_stream_pause(s));
    TEST_ASSERT_EQUAL(AUDIO_PLAYER_STATE_PAUSE, audio_stream_get_state(s));

    TEST_ESP_OK(audio_stream_resume(s));
    TEST_ASSERT_EQUAL(AUDIO_PLAYER_STATE_PLAYING, audio_stream_get_state(s));

    TEST_ESP_OK(audio_stream_delete(s));
}

TEST_CASE("audio stream queue", "[audio mixer]")
{
    audio_stream_config_t stream_cfg = DEFAULT_AUDIO_STREAM_CONFIG("queue");
    audio_stream_handle_t s = audio_stream_new(&stream_cfg);
    TEST_ASSERT_NOT_NULL(s);

    extern const char mp3_start[] asm("_binary_gs_16b_1c_44100hz_mp3_start");
    extern const char mp3_end[]   asm("_binary_gs_16b_1c_44100hz_mp3_end");
    size_t mp3_size = (size_t)((uintptr_t)mp3_end - (uintptr_t)mp3_start);

    FILE *f1 = fmemopen((void*)mp3_start, mp3_size, "rb");
    TEST_ASSERT_NOT_NULL(f1);

    TEST_ESP_OK(audio_stream_queue(s, f1, false));

    TEST_ESP_OK(audio_stream_delete(s));
}

TEST_CASE("audio stream write pcm", "[audio mixer]")
{
    audio_stream_config_t raw_cfg = {
        .type = AUDIO_STREAM_TYPE_RAW,
        .name = "raw_write",
        .priority = 5,
        .coreID = 0
    };
    audio_stream_handle_t s = audio_stream_new(&raw_cfg);
    TEST_ASSERT_NOT_NULL(s);

    int16_t dummy_pcm[128] = {0};
    TEST_ESP_OK(audio_stream_write_pcm(s, dummy_pcm, sizeof(dummy_pcm), 100));

    TEST_ESP_OK(audio_stream_delete(s));
}
