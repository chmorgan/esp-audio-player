/**
 * @file audio_http_stream.h
 * @brief HTTP audio streaming module
 *
 * This module provides HTTP/HTTPS audio streaming capabilities
 * with buffering and automatic reconnection support.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "audio_stream_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle for an HTTP audio stream
 */
typedef struct audio_http_stream* audio_http_stream_handle_t;

/**
 * @brief HTTP stream states
 */
typedef enum {
    AUDIO_HTTP_STREAM_STATE_IDLE = 0,
    AUDIO_HTTP_STREAM_STATE_CONNECTING,
    AUDIO_HTTP_STREAM_STATE_BUFFERING,
    AUDIO_HTTP_STREAM_STATE_PLAYING,
    AUDIO_HTTP_STREAM_STATE_PAUSED,
    AUDIO_HTTP_STREAM_STATE_ERROR,
    AUDIO_HTTP_STREAM_STATE_DISCONNECTED,
    AUDIO_HTTP_STREAM_STATE_FINISHED
} audio_http_stream_state_t;

/**
 * @brief HTTP stream events
 */
typedef enum {
    AUDIO_HTTP_STREAM_EVENT_CONNECTED = 0,
    AUDIO_HTTP_STREAM_EVENT_BUFFERING,
    AUDIO_HTTP_STREAM_EVENT_BUFFER_READY,
    AUDIO_HTTP_STREAM_EVENT_DISCONNECTED,
    AUDIO_HTTP_STREAM_EVENT_ERROR,
    AUDIO_HTTP_STREAM_EVENT_FINISHED
} audio_http_stream_event_t;

/**
 * @brief HTTP stream configuration
 */
typedef struct {
    const char *url;                  /**< HTTP/HTTPS URL to stream from */
    size_t buffer_size;               /**< Ring buffer size in bytes (default: 32768) */
    size_t low_watermark;             /**< Low watermark in bytes (default: 8192) */
    size_t high_watermark;            /**< High watermark in bytes (default: 24576) */
    uint32_t reconnect_timeout_ms;    /**< Reconnect timeout in ms (default: 5000) */
    uint32_t read_timeout_ms;         /**< Read timeout in ms (default: 10000) */
    uint32_t task_stack_size;         /**< HTTP task stack size (default: 4096) */
    UBaseType_t task_priority;        /**< HTTP task priority (default: tskIDLE_PRIORITY + 1) */
    BaseType_t task_core_id;          /**< HTTP task core ID (default: tskNO_AFFINITY) */
    const char **additional_headers;  /**< NULL-terminated array of additional headers, or NULL */
    bool enable_auto_reconnect;       /**< Enable automatic reconnection (default: true) */
} audio_http_stream_config_t;

/**
 * @brief Default HTTP stream configuration
 *
 * @param _url URL to stream from
 */
#define DEFAULT_AUDIO_HTTP_STREAM_CONFIG(_url) {  \
    .url = _url,                                   \
    .buffer_size = 32 * 1024,                     \
    .low_watermark = 8 * 1024,                    \
    .high_watermark = 24 * 1024,                  \
    .reconnect_timeout_ms = 5000,                  \
    .read_timeout_ms = 10000,                      \
    .task_stack_size = 4096,                       \
    .task_priority = tskIDLE_PRIORITY + 1,        \
    .task_core_id = tskNO_AFFINITY,                \
    .additional_headers = NULL,                     \
    .enable_auto_reconnect = true                   \
}

/**
 * @brief HTTP stream event callback type
 *
 * @param event Event that occurred
 * @param user_ctx User context provided during registration
 */
typedef void (*audio_http_stream_event_cb_t)(audio_http_stream_event_t event, void *user_ctx);

/**
 * @brief Open an HTTP stream
 *
 * @param cfg Stream configuration
 * @return Stream handle, or NULL on failure
 */
audio_http_stream_handle_t audio_http_stream_open(const audio_http_stream_config_t *cfg);

/**
 * @brief Get the stream I/O interface for an HTTP stream
 *
 * The I/O interface can be passed to audio_stream_play_from_io()
 *
 * The returned interface is single-consumer. It supports SEEK_SET and
 * SEEK_CUR only within the fully cached initial 8 KiB of data that has
 * already been consumed. Seeking into unread data and SEEK_END are not
 * supported, and failed seeks leave the position unchanged.
 *
 * @param h HTTP stream handle
 * @param io_out Output: pointer to receive the stream I/O handle
 * @return ESP_OK on success
 */
esp_err_t audio_http_stream_get_io(audio_http_stream_handle_t h, audio_stream_io_handle_t *io_out);

/**
 * @brief Register an event callback for an HTTP stream
 *
 * @param h HTTP stream handle
 * @param cb Callback function (can be NULL to unregister)
 * @param user_ctx User context passed to callback
 * @return ESP_OK on success
 */
esp_err_t audio_http_stream_register_cb(audio_http_stream_handle_t h,
                                          audio_http_stream_event_cb_t cb,
                                          void *user_ctx);

/**
 * @brief Get the current state of an HTTP stream
 *
 * @param h HTTP stream handle
 * @return Current state
 */
audio_http_stream_state_t audio_http_stream_get_state(audio_http_stream_handle_t h);

/**
 * @brief Get the number of bytes currently buffered
 *
 * @param h HTTP stream handle
 * @return Number of bytes buffered
 */
size_t audio_http_stream_get_buffered_bytes(audio_http_stream_handle_t h);

/**
 * @brief Get the total number of bytes downloaded
 *
 * @param h HTTP stream handle
 * @return Total bytes downloaded
 */
size_t audio_http_stream_get_total_bytes(audio_http_stream_handle_t h);

/**
 * @brief Get the content length from HTTP headers
 *
 * @param h HTTP stream handle
 * @return Content length in bytes, or -1 if unknown
 */
int audio_http_stream_get_content_length(audio_http_stream_handle_t h);

/**
 * @brief Pause the HTTP stream download
 *
 * Playback will continue until buffer is empty.
 *
 * @param h HTTP stream handle
 * @return ESP_OK on success
 */
esp_err_t audio_http_stream_pause(audio_http_stream_handle_t h);

/**
 * @brief Resume the HTTP stream download
 *
 * @param h HTTP stream handle
 * @return ESP_OK on success
 */
esp_err_t audio_http_stream_resume(audio_http_stream_handle_t h);

/**
 * @brief Close an HTTP stream and free all resources
 *
 * @param h HTTP stream handle (can be NULL)
 * @return ESP_OK on success
 */
esp_err_t audio_http_stream_close(audio_http_stream_handle_t h);

#ifdef __cplusplus
}
#endif
