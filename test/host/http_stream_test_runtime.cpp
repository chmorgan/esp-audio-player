#include "http_stream_test_runtime.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

#include "esp_http_client.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

struct host_test_task {
    TaskFunction_t function = nullptr;
    void *argument = nullptr;
    bool deleted = false;
};

struct returned_ring_item {
    std::vector<uint8_t> bytes;
    bool returned = false;
};

struct host_test_ring_buffer {
    explicit host_test_ring_buffer(std::size_t requested_capacity)
        : capacity(requested_capacity) {}

    std::size_t capacity;
    std::deque<uint8_t> queued;
    std::vector<std::unique_ptr<returned_ring_item>> received_items;
};

struct host_test_semaphore {};

struct host_test_http_client {
    esp_http_client_config_t config{};
    std::size_t offset = 0;
};

namespace {

host_test_task *latest_task;
host_test_ring_buffer *latest_ring_buffer;
std::vector<uint8_t> http_response;
std::size_t http_chunk_size;

}  // namespace

namespace http_stream_test_runtime {

void reset() {
    delete latest_task;
    latest_task = nullptr;
    latest_ring_buffer = nullptr;
    http_response.clear();
    http_chunk_size = 0;
}

bool feed_ring_buffer(const void *data, std::size_t size) {
    if (!latest_ring_buffer || (!data && size != 0)) {
        return false;
    }
    return xRingbufferSend(latest_ring_buffer, data, size, 0) == pdTRUE;
}

void configure_http_response(const void *data, std::size_t size, std::size_t chunk_size) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    if (size == 0) {
        http_response.clear();
    } else if (bytes) {
        http_response.assign(bytes, bytes + size);
    } else {
        http_response.clear();
    }
    http_chunk_size = chunk_size;
}

void run_captured_task() {
    if (latest_task && latest_task->function && !latest_task->deleted) {
        latest_task->function(latest_task->argument);
    }
}

}  // namespace http_stream_test_runtime

extern "C" {

// The stub declaration names every parameter, but cppcheck loses parameter
// names after the TaskFunction_t callback while matching this definition.
// cppcheck-suppress-begin funcArgNamesDifferentUnnamed
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task_function,
                                  const char *name,
                                  uint32_t stack_depth,
                                  void *argument,
                                  UBaseType_t priority,
                                  TaskHandle_t *created_task,
                                  BaseType_t core_id) {
    (void)name;
    (void)stack_depth;
    (void)priority;
    (void)core_id;

    delete latest_task;
    latest_task = new host_test_task{task_function, argument, false};
    if (created_task) {
        *created_task = latest_task;
    }
    return pdPASS;
}
// cppcheck-suppress-end funcArgNamesDifferentUnnamed

void vTaskDelete(TaskHandle_t task) {
    if (task) {
        task->deleted = true;
    } else if (latest_task) {
        latest_task->deleted = true;
    }
}

void vTaskDelay(TickType_t ticks) {
    (void)ticks;
}

RingbufHandle_t xRingbufferCreate(std::size_t size, int type) {
    (void)type;

    latest_ring_buffer = new host_test_ring_buffer(size);
    return latest_ring_buffer;
}

BaseType_t xRingbufferSend(RingbufHandle_t ring_buffer,
                           const void *data,
                           std::size_t size,
                           TickType_t ticks_to_wait) {
    (void)ticks_to_wait;

    if (!ring_buffer || (!data && size != 0) ||
        size > ring_buffer->capacity - ring_buffer->queued.size()) {
        return pdFALSE;
    }
    if (size == 0) {
        return pdTRUE;
    }

    const auto *bytes = static_cast<const uint8_t *>(data);
    ring_buffer->queued.insert(ring_buffer->queued.end(), bytes, bytes + size);
    return pdTRUE;
}

void *xRingbufferReceiveUpTo(RingbufHandle_t ring_buffer,
                             std::size_t *item_size,
                             TickType_t ticks_to_wait,
                             std::size_t max_size) {
    (void)ticks_to_wait;

    if (item_size) {
        *item_size = 0;
    }
    if (!ring_buffer || ring_buffer->queued.empty() || max_size == 0) {
        return nullptr;
    }

    const std::size_t size = std::min(max_size, ring_buffer->queued.size());
    auto item = std::make_unique<returned_ring_item>();
    item->bytes.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        item->bytes.push_back(ring_buffer->queued.front());
        ring_buffer->queued.pop_front();
    }

    void *data = item->bytes.data();
    ring_buffer->received_items.push_back(std::move(item));
    if (item_size) {
        *item_size = size;
    }
    return data;
}

void vRingbufferReturnItem(RingbufHandle_t ring_buffer, void *item) {
    if (!ring_buffer || !item) {
        return;
    }

    const auto returned_item = std::find_if(
        ring_buffer->received_items.begin(),
        ring_buffer->received_items.end(),
        [item](const std::unique_ptr<returned_ring_item> &candidate) {
            return candidate->bytes.data() == item;
        });
    if (returned_item == ring_buffer->received_items.end()) {
        return;
    }

    returned_ring_item &candidate = **returned_item;
    std::fill(candidate.bytes.begin(), candidate.bytes.end(), UINT8_C(0xA5));
    candidate.returned = true;
}

void vRingbufferGetInfo(RingbufHandle_t ring_buffer,
                        UBaseType_t *free,
                        UBaseType_t *read,
                        UBaseType_t *write,
                        UBaseType_t *acquire,
                        UBaseType_t *items_waiting) {
    (void)read;
    (void)write;
    (void)acquire;

    if (free) {
        *free = ring_buffer
                    ? static_cast<UBaseType_t>(ring_buffer->capacity - ring_buffer->queued.size())
                    : 0;
    }
    if (items_waiting) {
        *items_waiting =
            ring_buffer ? static_cast<UBaseType_t>(ring_buffer->queued.size()) : 0;
    }
}

void vRingbufferDelete(RingbufHandle_t ring_buffer) {
    if (ring_buffer == latest_ring_buffer) {
        latest_ring_buffer = nullptr;
    }
    delete ring_buffer;
}

SemaphoreHandle_t xSemaphoreCreateMutex() {
    return new host_test_semaphore;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks_to_wait) {
    (void)ticks_to_wait;

    return semaphore ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
    return semaphore ? pdTRUE : pdFALSE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore) {
    delete semaphore;
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    if (!config) {
        return nullptr;
    }
    auto *client = new host_test_http_client;
    client->config = *config;
    return client;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len) {
    (void)write_len;

    return client ? ESP_OK : ESP_FAIL;
}

int esp_http_client_fetch_headers(esp_http_client_handle_t client) {
    return client ? static_cast<int>(http_response.size()) : -1;
}

int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int buffer_len) {
    if (!client || !buffer || buffer_len < 0) {
        return -1;
    }
    if (client->offset >= http_response.size()) {
        return 0;
    }

    std::size_t size = std::min<std::size_t>(
        static_cast<std::size_t>(buffer_len), http_response.size() - client->offset);
    if (http_chunk_size != 0) {
        size = std::min(size, http_chunk_size);
    }
    std::memcpy(buffer, http_response.data() + client->offset, size);
    client->offset += size;
    return static_cast<int>(size);
}

esp_err_t esp_http_client_close(esp_http_client_handle_t client) {
    return client ? ESP_OK : ESP_FAIL;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client) {
    delete client;
    return ESP_OK;
}

const char *esp_err_to_name(esp_err_t error) {
    return error == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}

}  // extern "C"
