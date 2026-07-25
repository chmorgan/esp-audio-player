/**
 * @file audio_http_stream.cpp
 * @brief HTTP audio streaming implementation
 */

#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"

#include "audio_http_stream.h"
#include "audio_log.h"

#if CONFIG_AUDIO_PLAYER_ENABLE_HTTP_STREAM
#include "esp_http_client.h"
#endif

static const char *TAG = "audio_http_stream";

/*
 * audio_http_stream_t is allocated with calloc() in
 * audio_http_stream_open(). Members intentionally start at zero except
 * content_length, which is explicitly initialized to -1 after allocation.
 */
// cppcheck-suppress-begin uninitMemberVarNoCtor
typedef struct audio_http_stream {
    audio_http_stream_config_t cfg;
    audio_http_stream_state_t state;
    audio_http_stream_event_cb_t event_cb;
    void *event_cb_ctx;

    RingbufHandle_t ringbuf;
    SemaphoreHandle_t mutex;
    TaskHandle_t task;
    bool task_running;
    bool paused;
    bool eof_reached;
    bool error_occurred;
    bool id3_tag_skipped;

    size_t total_bytes_downloaded;
    int content_length;  // Total content length from HTTP headers, -1 if unknown

    audio_stream_io_handle_t io_handle;

    // Logical read position and the source position at the head of the ring.
    // A cursor behind the frontier is replaying the cached source prefix.
    size_t cursor;
    size_t frontier;

    // Seek support: keep an exact source prefix to allow bounded rewinding.
    uint8_t *initial_buf;
    size_t initial_buf_size;
    size_t initial_buf_filled;
} audio_http_stream_t;
// cppcheck-suppress-end uninitMemberVarNoCtor

/* ================= Stream I/O callbacks for HTTP stream ================= */

static size_t http_stream_buffered_bytes(const audio_http_stream_t *stream) {
    if (!stream || !stream->ringbuf) return 0;

    UBaseType_t items_waiting = 0;
    vRingbufferGetInfo(stream->ringbuf, NULL, NULL, NULL, NULL, &items_waiting);
    return static_cast<size_t>(items_waiting);
}

static void http_stream_get_terminal_state(audio_http_stream_t *stream,
                                           bool *eof_reached,
                                           bool *error_occurred) {
    if (stream->mutex) xSemaphoreTake(stream->mutex, portMAX_DELAY);
    *eof_reached = stream->eof_reached;
    *error_occurred = stream->error_occurred;
    if (stream->mutex) xSemaphoreGive(stream->mutex);
}

static void http_stream_set_eof(audio_http_stream_t *stream, bool reached) {
    if (stream->mutex) xSemaphoreTake(stream->mutex, portMAX_DELAY);
    stream->eof_reached = reached;
    if (stream->mutex) xSemaphoreGive(stream->mutex);
}

static void http_stream_set_error(audio_http_stream_t *stream, bool occurred) {
    if (stream->mutex) xSemaphoreTake(stream->mutex, portMAX_DELAY);
    stream->error_occurred = occurred;
    if (stream->mutex) xSemaphoreGive(stream->mutex);
}

static size_t http_stream_read(void *ctx, void *buf, size_t size) {
    audio_http_stream_t *stream = static_cast<audio_http_stream_t*>(ctx);
    if (!stream || !stream->ringbuf || (!buf && size != 0)) return 0;

    size_t total_read = 0;
    uint8_t *dst = static_cast<uint8_t*>(buf);

    // Replay only bytes explicitly sought behind the source frontier. Normal
    // sequential reads keep cursor == frontier and therefore never replay.
    while (total_read < size && stream->cursor < stream->frontier) {
        if (!stream->initial_buf || stream->cursor >= stream->initial_buf_filled) {
            ESP_LOGE(TAG, "HTTP replay cursor is outside the cached prefix");
            return total_read;
        }

        size_t replay_end = stream->frontier;
        if (replay_end > stream->initial_buf_filled) {
            replay_end = stream->initial_buf_filled;
        }

        size_t to_copy = replay_end - stream->cursor;
        if (to_copy > size - total_read) {
            to_copy = size - total_read;
        }
        memcpy(dst + total_read, stream->initial_buf + stream->cursor, to_copy);
        total_read += to_copy;
        stream->cursor += to_copy;
    }

    // Once replay catches the frontier, continue with bytes from the ring.
    // Retry a few times if the producer is still active and temporarily empty.
    int retry_count = 0;
    const int max_retries = 10;

    while (total_read < size && retry_count < max_retries) {
        bool eof_reached = false;
        bool error_occurred = false;
        http_stream_get_terminal_state(stream, &eof_reached, &error_occurred);
        const bool producer_terminal = eof_reached || error_occurred;

        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(stream->ringbuf, &item_size,
                                              producer_terminal ? 0 : pdMS_TO_TICKS(200),
                                              size - total_read);

        if (item && item_size > 0) {
            if (item_size > SIZE_MAX - stream->frontier) {
                ESP_LOGE(TAG, "HTTP stream position overflow");
                vRingbufferReturnItem(stream->ringbuf, item);
                break;
            }

            memcpy(dst + total_read, item, item_size);

            // Keep an exact copy of the source prefix. The ring item must be
            // copied before it is returned to FreeRTOS.
            if (stream->initial_buf &&
                stream->initial_buf_filled == stream->frontier &&
                stream->initial_buf_filled < stream->initial_buf_size) {
                size_t to_save = item_size;
                if (to_save > stream->initial_buf_size - stream->initial_buf_filled) {
                    to_save = stream->initial_buf_size - stream->initial_buf_filled;
                }
                if (to_save > 0) {
                    memcpy(stream->initial_buf + stream->initial_buf_filled, item, to_save);
                    stream->initial_buf_filled += to_save;
                }
            }

            total_read += item_size;
            stream->frontier += item_size;
            stream->cursor = stream->frontier;
            vRingbufferReturnItem(stream->ringbuf, item);
            retry_count = 0;  // Reset retry count on successful read
        } else if (item) {
            vRingbufferReturnItem(stream->ringbuf, item);
        } else if (producer_terminal) {
            break;
        } else {
            // Ringbuffer empty - increment retry count and yield to allow download task to fill buffer
            retry_count++;
            if (retry_count < max_retries) {
                ESP_LOGD(TAG, "ringbuf empty, retry %d/%d", retry_count, max_retries);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
    }

    if (retry_count >= max_retries && total_read == 0) {
        ESP_LOGW(TAG, "http_stream_read: gave up after %d retries, returning 0", max_retries);
    }

    return total_read;
}

static int http_stream_seek(void *ctx, long offset, int whence) {
    audio_http_stream_t *stream = static_cast<audio_http_stream_t*>(ctx);
    if (!stream) return -1;

    size_t target = 0;
    switch (whence) {
        case AUDIO_STREAM_SEEK_SET:
            if (offset < 0 ||
                static_cast<uintmax_t>(offset) > static_cast<uintmax_t>(SIZE_MAX)) {
                return -1;
            }
            target = static_cast<size_t>(offset);
            break;

        case AUDIO_STREAM_SEEK_CUR:
            if (offset < 0) {
                // Avoid negating LONG_MIN.
                const uintmax_t magnitude =
                    static_cast<uintmax_t>(-(offset + 1)) + UINTMAX_C(1);
                if (magnitude > static_cast<uintmax_t>(stream->cursor)) {
                    return -1;
                }
                target = stream->cursor - static_cast<size_t>(magnitude);
            } else {
                const uintmax_t increment = static_cast<uintmax_t>(offset);
                if (increment >
                    static_cast<uintmax_t>(SIZE_MAX - stream->cursor)) {
                    return -1;
                }
                target = stream->cursor + static_cast<size_t>(increment);
            }
            break;

        case AUDIO_STREAM_SEEK_END:
        default:
            return -1;
    }

    // A no-op remains valid even after the replay window has been exceeded.
    if (target == stream->cursor) {
        return 0;
    }

    // HTTP seek is limited to the fully cached consumed prefix. It never
    // blocks to consume unread network data, and failed seeks do not move.
    if (!stream->initial_buf ||
        stream->frontier > stream->initial_buf_filled ||
        target > stream->frontier) {
        return -1;
    }

    stream->cursor = target;
    return 0;
}

static long http_stream_tell(void *ctx) {
    const audio_http_stream_t *stream = static_cast<audio_http_stream_t*>(ctx);
    if (!stream ||
        static_cast<uintmax_t>(stream->cursor) >
            static_cast<uintmax_t>(LONG_MAX)) {
        return -1;
    }
    return static_cast<long>(stream->cursor);
}

static int http_stream_eof(void *ctx) {
    audio_http_stream_t *stream = static_cast<audio_http_stream_t*>(ctx);
    if (!stream) return 1;

    bool eof_reached = false;
    bool error_occurred = false;
    http_stream_get_terminal_state(stream, &eof_reached, &error_occurred);
    const bool replay_available = stream->cursor < stream->frontier;
    return (eof_reached || error_occurred) &&
           !replay_available &&
           http_stream_buffered_bytes(stream) == 0;
}

static void http_stream_close(void *ctx) {
    (void)ctx;
}

static const audio_stream_io_ops_t http_stream_io_ops = {
    .read = http_stream_read,
    .seek = http_stream_seek,
    .tell = http_stream_tell,
    .eof = http_stream_eof,
    .close = http_stream_close
};

/* ================= Event dispatch ================= */

static void dispatch_event(audio_http_stream_t *stream, audio_http_stream_event_t event) {
    if (stream->event_cb) {
        stream->event_cb(event, stream->event_cb_ctx);
    }
}

/* ================= HTTP download task ================= */

#if CONFIG_AUDIO_PLAYER_ENABLE_HTTP_STREAM

// ESP-IDF defines esp_http_client_event_cb_t with a mutable event pointer.
// cppcheck-suppress constParameterCallback
static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}

static void http_download_task(void *arg) {
    audio_http_stream_t *stream = static_cast<audio_http_stream_t*>(arg);
    esp_http_client_handle_t client = NULL;
    esp_err_t err;

    stream->state = AUDIO_HTTP_STREAM_STATE_CONNECTING;

    while (stream->task_running) {
        if (stream->paused) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!client) {
            ESP_LOGI(TAG, "Creating HTTP client for URL: %s", stream->cfg.url);
            // Log URL length and first few bytes to check for corruption
            ESP_LOGI(TAG, "URL length: %d, first bytes: 0x%02X 0x%02X 0x%02X 0x%02X",
                     strlen(stream->cfg.url),
                     (unsigned char)stream->cfg.url[0],
                     (unsigned char)stream->cfg.url[1],
                     (unsigned char)stream->cfg.url[2],
                     (unsigned char)stream->cfg.url[3]);
            esp_http_client_config_t config = {};
            config.url = stream->cfg.url;
            config.event_handler = http_event_handler;
            config.timeout_ms = stream->cfg.read_timeout_ms;
            config.keep_alive_enable = false;
            config.skip_cert_common_name_check = true;

            ESP_LOGI(TAG, "Calling esp_http_client_init...");
            client = esp_http_client_init(&config);
            if (!client) {
                ESP_LOGE(TAG, "Failed to initialize HTTP client for URL: %s", stream->cfg.url);
                stream->state = AUDIO_HTTP_STREAM_STATE_ERROR;
                http_stream_set_error(stream, true);
                dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_ERROR);

                if (stream->cfg.enable_auto_reconnect) {
                    vTaskDelay(pdMS_TO_TICKS(stream->cfg.reconnect_timeout_ms));
                    continue;
                }
                break;
            }

            ESP_LOGI(TAG, "HTTP client initialized, opening connection...");
            err = esp_http_client_open(client, 0);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to open HTTP connection: %s, URL: %s", esp_err_to_name(err), stream->cfg.url);
                esp_http_client_cleanup(client);
                client = NULL;
                stream->state = AUDIO_HTTP_STREAM_STATE_ERROR;
                dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_DISCONNECTED);

                if (stream->cfg.enable_auto_reconnect) {
                    vTaskDelay(pdMS_TO_TICKS(stream->cfg.reconnect_timeout_ms));
                    continue;
                }
                break;
            }
            ESP_LOGI(TAG, "HTTP connection opened successfully");

            stream->state = AUDIO_HTTP_STREAM_STATE_BUFFERING;
            dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_CONNECTED);

            int content_length = esp_http_client_fetch_headers(client);
            stream->content_length = content_length;
            ESP_LOGI(TAG, "Content length: %d", content_length);
        }

        uint8_t *buf = static_cast<uint8_t*>(malloc(1024));
        if (!buf) {
            ESP_LOGE(TAG, "Failed to allocate read buffer");
            break;
        }

        int read_len = esp_http_client_read(client, reinterpret_cast<char*>(buf), 1024);

        // Debug: log every read attempt result
        ESP_LOGD(TAG, "HTTP read result: read_len=%d, total_downloaded=%d", read_len, (int)stream->total_bytes_downloaded);

        if (read_len > 0) {
            size_t written = 0;
            while (written < static_cast<size_t>(read_len)) {
                size_t to_write = read_len - written;
                BaseType_t res = xRingbufferSend(stream->ringbuf, buf + written, to_write, pdMS_TO_TICKS(100));
                if (res == pdTRUE) {
                    written += to_write;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }

            stream->total_bytes_downloaded += read_len;

            const size_t bytes_available = http_stream_buffered_bytes(stream);

            LOGI_2(TAG, "HTTP download: read=%d, total=%d, buffered=%d",
                    read_len, (int)stream->total_bytes_downloaded, (int)bytes_available);

            if (stream->state == AUDIO_HTTP_STREAM_STATE_BUFFERING &&
                bytes_available >= stream->cfg.high_watermark) {
                stream->state = AUDIO_HTTP_STREAM_STATE_PLAYING;
                dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_BUFFER_READY);
            }
        } else if (read_len == 0) {
            ESP_LOGI(TAG, "End of HTTP stream");
            http_stream_set_eof(stream, true);
            stream->state = AUDIO_HTTP_STREAM_STATE_FINISHED;
            dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_FINISHED);
            break;
        } else {
            ESP_LOGE(TAG, "HTTP read error: %d", read_len);
            esp_http_client_cleanup(client);
            client = NULL;
            stream->state = AUDIO_HTTP_STREAM_STATE_ERROR;
            http_stream_set_error(stream, true);
            dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_ERROR);

            if (stream->cfg.enable_auto_reconnect) {
                vTaskDelay(pdMS_TO_TICKS(stream->cfg.reconnect_timeout_ms));
                http_stream_set_error(stream, false);
                continue;
            }
            break;
        }

        free(buf);

        if (stream->state == AUDIO_HTTP_STREAM_STATE_PLAYING &&
            http_stream_buffered_bytes(stream) < stream->cfg.low_watermark) {
            stream->state = AUDIO_HTTP_STREAM_STATE_BUFFERING;
            dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_BUFFERING);
        }
    }

    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    stream->task = NULL;
    vTaskDelete(NULL);
}

#else

static void http_download_task(void *arg) {
    audio_http_stream_t *stream = static_cast<audio_http_stream_t*>(arg);
    ESP_LOGE(TAG, "HTTP streaming not enabled. Set CONFIG_AUDIO_PLAYER_ENABLE_HTTP_STREAM=y");
    stream->state = AUDIO_HTTP_STREAM_STATE_ERROR;
    http_stream_set_error(stream, true);
    dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_ERROR);
    stream->task = NULL;
    vTaskDelete(NULL);
}

#endif

/* ================= Public API ================= */

audio_http_stream_handle_t audio_http_stream_open(const audio_http_stream_config_t *cfg) {
    ESP_RETURN_ON_FALSE(cfg != NULL, NULL, TAG, "cfg is NULL");
    ESP_RETURN_ON_FALSE(cfg->url != NULL, NULL, TAG, "url is NULL");

    audio_http_stream_t *stream = static_cast<audio_http_stream_t*>(calloc(1, sizeof(audio_http_stream_t)));
    ESP_RETURN_ON_FALSE(stream != NULL, NULL, TAG, "allocation failed");

    stream->content_length = -1;

    // Copy the URL string since caller may free it
    char *url_copy = strdup(cfg->url);
    if (!url_copy) {
        ESP_LOGE(TAG, "Failed to copy URL string");
        free(stream);
        return NULL;
    }

    stream->cfg = *cfg;
    stream->cfg.url = url_copy;

    if (stream->cfg.buffer_size == 0) {
        stream->cfg.buffer_size = 32 * 1024;
    }
    if (stream->cfg.low_watermark == 0) {
        stream->cfg.low_watermark = 8 * 1024;
    }
    if (stream->cfg.high_watermark == 0) {
        stream->cfg.high_watermark = 24 * 1024;
    }
    if (stream->cfg.low_watermark >= stream->cfg.high_watermark) {
        stream->cfg.low_watermark = stream->cfg.high_watermark / 2;
    }
    if (stream->cfg.task_stack_size == 0) {
        stream->cfg.task_stack_size = 4096;
    }
    if (stream->cfg.task_priority == 0) {
        stream->cfg.task_priority = tskIDLE_PRIORITY + 1;
    }

    stream->ringbuf = xRingbufferCreate(stream->cfg.buffer_size, RINGBUF_TYPE_BYTEBUF);
    if (!stream->ringbuf) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        free(url_copy);
        free(stream);
        return NULL;
    }

    stream->mutex = xSemaphoreCreateMutex();
    if (!stream->mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        vRingbufferDelete(stream->ringbuf);
        free(url_copy);
        free(stream);
        return NULL;
    }

    stream->state = AUDIO_HTTP_STREAM_STATE_IDLE;
    stream->task_running = true;

    stream->io_handle = audio_stream_io_create(&http_stream_io_ops, stream);
    if (!stream->io_handle) {
        ESP_LOGE(TAG, "Failed to create stream I/O");
        vSemaphoreDelete(stream->mutex);
        vRingbufferDelete(stream->ringbuf);
        free(url_copy);
        free(stream);
        return NULL;
    }

    // Allocate initial buffer for seek support (8KB to cover MP3 headers)
    stream->initial_buf_size = 8 * 1024;
    stream->initial_buf = static_cast<uint8_t*>(malloc(stream->initial_buf_size));
    if (!stream->initial_buf) {
        ESP_LOGW(TAG, "Failed to allocate initial buffer, seek will not be supported");
        stream->initial_buf_size = 0;
    }
    stream->initial_buf_filled = 0;

    BaseType_t res = xTaskCreatePinnedToCore(
        http_download_task,
        "audio_http",
        stream->cfg.task_stack_size,
        stream,
        stream->cfg.task_priority,
        &stream->task,
        stream->cfg.task_core_id
    );

    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create download task");
        audio_stream_io_close(stream->io_handle);
        vSemaphoreDelete(stream->mutex);
        vRingbufferDelete(stream->ringbuf);
        free(url_copy);
        free(stream);
        return NULL;
    }

    return stream;
}

esp_err_t audio_http_stream_get_io(audio_http_stream_handle_t h, audio_stream_io_handle_t *io_out) {
    ESP_RETURN_ON_FALSE(h != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(io_out != NULL, ESP_ERR_INVALID_ARG, TAG, "io_out is NULL");

    *io_out = h->io_handle;
    return ESP_OK;
}

esp_err_t audio_http_stream_register_cb(audio_http_stream_handle_t h,
                                          audio_http_stream_event_cb_t cb,
                                          void *user_ctx) {
    ESP_RETURN_ON_FALSE(h != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    if (h->mutex) xSemaphoreTake(h->mutex, portMAX_DELAY);
    h->event_cb = cb;
    h->event_cb_ctx = user_ctx;
    if (h->mutex) xSemaphoreGive(h->mutex);

    return ESP_OK;
}

audio_http_stream_state_t audio_http_stream_get_state(audio_http_stream_handle_t h) {
    if (!h) return AUDIO_HTTP_STREAM_STATE_ERROR;
    return h->state;
}

size_t audio_http_stream_get_buffered_bytes(audio_http_stream_handle_t h) {
    return http_stream_buffered_bytes(h);
}

size_t audio_http_stream_get_total_bytes(audio_http_stream_handle_t h) {
    if (!h) return 0;
    return h->total_bytes_downloaded;
}

int audio_http_stream_get_content_length(audio_http_stream_handle_t h) {
    if (!h) return -1;
    return h->content_length;
}

esp_err_t audio_http_stream_pause(audio_http_stream_handle_t h) {
    ESP_RETURN_ON_FALSE(h != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    h->paused = true;
    if (h->state == AUDIO_HTTP_STREAM_STATE_PLAYING ||
        h->state == AUDIO_HTTP_STREAM_STATE_BUFFERING) {
        h->state = AUDIO_HTTP_STREAM_STATE_PAUSED;
    }

    return ESP_OK;
}

esp_err_t audio_http_stream_resume(audio_http_stream_handle_t h) {
    ESP_RETURN_ON_FALSE(h != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    h->paused = false;
    if (h->state == AUDIO_HTTP_STREAM_STATE_PAUSED) {
        h->state = AUDIO_HTTP_STREAM_STATE_BUFFERING;
    }

    return ESP_OK;
}

esp_err_t audio_http_stream_close(audio_http_stream_handle_t h) {
    if (!h) return ESP_OK;

    h->task_running = false;

    if (h->task) {
        vTaskDelete(h->task);
        h->task = NULL;
    }

    if (h->io_handle) {
        // Don't close io_handle here - it's owned and closed by the audio player
        h->io_handle = NULL;
    }

    if (h->ringbuf) {
        vRingbufferDelete(h->ringbuf);
        h->ringbuf = NULL;
    }

    if (h->mutex) {
        vSemaphoreDelete(h->mutex);
        h->mutex = NULL;
    }

    if (h->initial_buf) {
        free(h->initial_buf);
        h->initial_buf = NULL;
    }

    if (h->cfg.url) {
        free(const_cast<char *>(h->cfg.url));
        h->cfg.url = NULL;
    }

    free(h);
    return ESP_OK;
}
