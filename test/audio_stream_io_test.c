/**
 * @file audio_stream_io_test.c
 * @brief Unit tests for audio_stream_io and HTTP streaming
 */

#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "unity.h"
#include "audio_player.h"
#include "audio_mixer.h"
#include "audio_stream.h"
#include "audio_stream_io.h"
#include "freertos/semphr.h"

static const char *TAG = "STREAM_IO_TEST";

/* ==================== Memory stream tests ==================== */

TEST_CASE("audio_stream_io can create from memory buffer", "[stream_io]")
{
    const uint8_t test_data[] = "Hello, World!";
    size_t test_size = sizeof(test_data);

    audio_stream_io_handle_t io = audio_stream_io_from_memory(test_data, test_size, false);
    TEST_ASSERT_NOT_NULL(io);

    uint8_t read_buf[32] = {0};
    size_t read = audio_stream_io_read(io, read_buf, test_size);
    TEST_ASSERT_EQUAL(test_size, read);
    TEST_ASSERT_EQUAL_MEMORY(test_data, read_buf, test_size);

    audio_stream_io_close(io);
}

TEST_CASE("audio_stream_io from memory copies buffer when requested", "[stream_io]")
{
    uint8_t test_data[] = "Test data";
    size_t test_size = sizeof(test_data);

    audio_stream_io_handle_t io = audio_stream_io_from_memory(test_data, test_size, true);
    TEST_ASSERT_NOT_NULL(io);

    test_data[0] = 'X';

    uint8_t read_buf[32] = {0};
    audio_stream_io_read(io, read_buf, test_size);
    TEST_ASSERT_EQUAL('T', read_buf[0]);

    audio_stream_io_close(io);
}

TEST_CASE("audio_stream_io can seek and tell in memory stream", "[stream_io]")
{
    const uint8_t test_data[] = "0123456789";
    size_t test_size = sizeof(test_data);

    audio_stream_io_handle_t io = audio_stream_io_from_memory(test_data, test_size, false);
    TEST_ASSERT_NOT_NULL(io);

    long pos;
    TEST_ESP_OK(audio_stream_io_tell(io, &pos));
    TEST_ASSERT_EQUAL(0, pos);

    uint8_t buf[4];
    size_t read = audio_stream_io_read(io, buf, 3);
    TEST_ASSERT_EQUAL(3, read);
    TEST_ASSERT_EQUAL('0', buf[0]);
    TEST_ASSERT_EQUAL('1', buf[1]);
    TEST_ASSERT_EQUAL('2', buf[2]);

    TEST_ESP_OK(audio_stream_io_tell(io, &pos));
    TEST_ASSERT_EQUAL(3, pos);

    TEST_ESP_OK(audio_stream_io_seek(io, 5, AUDIO_STREAM_SEEK_SET));
    TEST_ESP_OK(audio_stream_io_tell(io, &pos));
    TEST_ASSERT_EQUAL(5, pos);

    read = audio_stream_io_read(io, buf, 2);
    TEST_ASSERT_EQUAL(2, read);
    TEST_ASSERT_EQUAL('5', buf[0]);
    TEST_ASSERT_EQUAL('6', buf[1]);

    TEST_ESP_OK(audio_stream_io_seek(io, -2, AUDIO_STREAM_SEEK_CUR));
    TEST_ESP_OK(audio_stream_io_tell(io, &pos));
    TEST_ASSERT_EQUAL(5, pos);

    TEST_ESP_OK(audio_stream_io_seek(io, -1, AUDIO_STREAM_SEEK_END));
    TEST_ESP_OK(audio_stream_io_tell(io, &pos));
    TEST_ASSERT_EQUAL(test_size - 1, pos);

    audio_stream_io_close(io);
}

TEST_CASE("audio_stream_io reports EOF correctly", "[stream_io]")
{
    const uint8_t test_data[] = "ABC";
    size_t test_size = sizeof(test_data);

    audio_stream_io_handle_t io = audio_stream_io_from_memory(test_data, test_size, false);
    TEST_ASSERT_NOT_NULL(io);

    TEST_ASSERT_FALSE(audio_stream_io_eof(io));

    uint8_t buf[10];
    size_t read = audio_stream_io_read(io, buf, test_size);
    TEST_ASSERT_EQUAL(test_size, read);
    TEST_ASSERT_FALSE(audio_stream_io_eof(io));

    read = audio_stream_io_read(io, buf, 10);
    TEST_ASSERT_EQUAL(0, read);
    TEST_ASSERT_TRUE(audio_stream_io_eof(io));

    audio_stream_io_close(io);
}

/* ==================== File stream tests ==================== */

TEST_CASE("audio_stream_io can create from FILE*", "[stream_io]")
{
    extern const char mp3_start[] asm("_binary_gs_16b_1c_44100hz_mp3_start");
    extern const char mp3_end[]   asm("_binary_gs_16b_1c_44100hz_mp3_end");
    size_t mp3_size = (size_t)((uintptr_t)mp3_end - (uintptr_t)mp3_start);

    FILE *f = fmemopen((void*)mp3_start, mp3_size, "rb");
    TEST_ASSERT_NOT_NULL(f);

    audio_stream_io_handle_t io = audio_stream_io_from_file(f);
    TEST_ASSERT_NOT_NULL(io);

    uint8_t magic[3];
    size_t read = audio_stream_io_read(io, magic, 3);
    TEST_ASSERT_EQUAL(3, read);
    TEST_ASSERT_EQUAL(mp3_start[0], magic[0]);
    TEST_ASSERT_EQUAL(mp3_start[1], magic[1]);
    TEST_ASSERT_EQUAL(mp3_start[2], magic[2]);

    audio_stream_io_close(io);
}

TEST_CASE("audio_stream_io from FILE* no_close doesn't close file", "[stream_io]")
{
    const uint8_t test_data[] = "Test file data";
    size_t test_size = sizeof(test_data);

    FILE *f = fmemopen((void*)test_data, test_size, "rb");
    TEST_ASSERT_NOT_NULL(f);

    audio_stream_io_handle_t io = audio_stream_io_from_file_no_close(f);
    TEST_ASSERT_NOT_NULL(io);

    audio_stream_io_close(io);

    uint8_t buf[16];
    rewind(f);
    size_t read = fread(buf, 1, test_size, f);
    TEST_ASSERT_EQUAL(test_size, read);
    TEST_ASSERT_EQUAL_MEMORY(test_data, buf, test_size);

    fclose(f);
}

/* ==================== Custom stream tests ==================== */

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
} test_stream_ctx_t;

static size_t test_stream_read(void *ctx, void *buf, size_t size) {
    test_stream_ctx_t *tctx = (test_stream_ctx_t*)ctx;
    if (!tctx || !tctx->data) return 0;

    size_t available = tctx->size - tctx->pos;
    size_t to_read = (size < available) ? size : available;

    if (to_read > 0) {
        memcpy(buf, tctx->data + tctx->pos, to_read);
        tctx->pos += to_read;
    }

    return to_read;
}

static int test_stream_seek(void *ctx, long offset, int whence) {
    test_stream_ctx_t *tctx = (test_stream_ctx_t*)ctx;
    if (!tctx) return -1;

    size_t new_pos;
    switch (whence) {
        case AUDIO_STREAM_SEEK_SET:
            new_pos = offset;
            break;
        case AUDIO_STREAM_SEEK_CUR:
            new_pos = tctx->pos + offset;
            break;
        case AUDIO_STREAM_SEEK_END:
            new_pos = tctx->size + offset;
            break;
        default:
            return -1;
    }

    if (new_pos > tctx->size) return -1;
    tctx->pos = new_pos;
    return 0;
}

// audio_stream_io_ops_t defines callbacks with a mutable context pointer.
// cppcheck-suppress constParameterCallback
static long test_stream_tell(void *ctx) {
    const test_stream_ctx_t *tctx = ctx;
    if (!tctx) return -1;
    return (long)tctx->pos;
}

// audio_stream_io_ops_t defines callbacks with a mutable context pointer.
// cppcheck-suppress constParameterCallback
static int test_stream_eof(void *ctx) {
    const test_stream_ctx_t *tctx = ctx;
    if (!tctx) return 1;
    return (tctx->pos >= tctx->size) ? 1 : 0;
}

static void test_stream_close(void *ctx) {
    free(ctx);
}

TEST_CASE("audio_stream_io can use custom stream implementation", "[stream_io]")
{
    static const audio_stream_io_ops_t test_ops = {
        .read = test_stream_read,
        .seek = test_stream_seek,
        .tell = test_stream_tell,
        .eof = test_stream_eof,
        .close = test_stream_close
    };

    test_stream_ctx_t *ctx = (test_stream_ctx_t*)malloc(sizeof(test_stream_ctx_t));
    if (ctx == NULL) {
        TEST_FAIL_MESSAGE("Failed to allocate custom stream context");
        return;
    }

    ctx->data = (const uint8_t*)"Custom stream test";
    ctx->size = strlen((char*)ctx->data) + 1;
    ctx->pos = 0;

    audio_stream_io_handle_t io = audio_stream_io_create(&test_ops, ctx);
    TEST_ASSERT_NOT_NULL(io);

    uint8_t buf[32];
    size_t read = audio_stream_io_read(io, buf, ctx->size);
    TEST_ASSERT_EQUAL(ctx->size, read);
    TEST_ASSERT_EQUAL_STRING("Custom stream test", (char*)buf);

    audio_stream_io_close(io);
}

/* ==================== Integration with audio stream ==================== */

TEST_CASE("audio_stream can play from memory stream", "[stream_io][integration]")
{
    extern const char mp3_start[] asm("_binary_gs_16b_1c_44100hz_mp3_start");
    extern const char mp3_end[]   asm("_binary_gs_16b_1c_44100hz_mp3_end");
    size_t mp3_size = (size_t)((uintptr_t)mp3_end - (uintptr_t)mp3_start);

    audio_stream_config_t stream_cfg = DEFAULT_AUDIO_STREAM_CONFIG("memory_stream_test");
    audio_stream_handle_t stream = audio_stream_new(&stream_cfg);
    TEST_ASSERT_NOT_NULL(stream);

    audio_stream_io_handle_t io = audio_stream_io_from_memory(mp3_start, mp3_size, true);
    TEST_ASSERT_NOT_NULL(io);

    TEST_ESP_OK(audio_stream_play_io(stream, io));

    vTaskDelay(pdMS_TO_TICKS(100));

    TEST_ESP_OK(audio_stream_stop(stream));
    TEST_ESP_OK(audio_stream_delete(stream));
}

/* ==================== HTTP stream placeholder tests ==================== */

#if CONFIG_AUDIO_PLAYER_ENABLE_HTTP_STREAM

TEST_CASE("audio_http_stream can be configured with defaults", "[http_stream]")
{
    audio_http_stream_config_t cfg = DEFAULT_AUDIO_HTTP_STREAM_CONFIG("http://example.com/test.mp3");

    TEST_ASSERT_NOT_NULL(cfg.url);
    TEST_ASSERT_EQUAL_STRING("http://example.com/test.mp3", cfg.url);
    TEST_ASSERT_EQUAL(32 * 1024, cfg.buffer_size);
    TEST_ASSERT_EQUAL(8 * 1024, cfg.low_watermark);
    TEST_ASSERT_EQUAL(24 * 1024, cfg.high_watermark);
    TEST_ASSERT_TRUE(cfg.enable_auto_reconnect);
}

TEST_CASE("audio_http_stream validates watermark configuration", "[http_stream]")
{
    audio_http_stream_config_t cfg = DEFAULT_AUDIO_HTTP_STREAM_CONFIG("http://example.com/test.mp3");

    cfg.low_watermark = cfg.high_watermark + 1000;

    audio_http_stream_handle_t stream = audio_http_stream_open(&cfg);
    TEST_ASSERT_NOT_NULL(stream);

    audio_http_stream_close(stream);
}

#endif
