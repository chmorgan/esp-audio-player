#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HTTP_EVENT_ERROR = 0,
    HTTP_EVENT_ON_CONNECTED,
    HTTP_EVENT_HEADER_SENT,
    HTTP_EVENT_ON_HEADER,
    HTTP_EVENT_ON_DATA,
    HTTP_EVENT_ON_FINISH,
    HTTP_EVENT_DISCONNECTED,
    HTTP_EVENT_REDIRECT
} esp_http_client_event_id_t;

typedef struct esp_http_client_event {
    esp_http_client_event_id_t event_id;
    const char *header_key;
    const char *header_value;
    void *data;
    int data_len;
} esp_http_client_event_t;

typedef esp_err_t (*esp_http_client_event_cb_t)(esp_http_client_event_t *event);

typedef struct {
    const char *url;
    esp_http_client_event_cb_t event_handler;
    int timeout_ms;
    bool keep_alive_enable;
    bool skip_cert_common_name_check;
} esp_http_client_config_t;

struct host_test_http_client;
typedef struct host_test_http_client *esp_http_client_handle_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
int esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int buffer_len);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
const char *esp_err_to_name(esp_err_t error);

#ifdef __cplusplus
}
#endif
