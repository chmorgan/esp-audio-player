#pragma once

#include <cstddef>

namespace http_stream_test_runtime {

void reset();
bool feed_ring_buffer(const void *data, std::size_t size);
void configure_http_response(const void *data, std::size_t size, std::size_t chunk_size);
void run_captured_task();

}  // namespace http_stream_test_runtime
