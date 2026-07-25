#pragma once

#include <stddef.h>

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

struct host_test_ring_buffer;

typedef struct host_test_ring_buffer *RingbufHandle_t;

#define RINGBUF_TYPE_BYTEBUF 0

RingbufHandle_t xRingbufferCreate(size_t size, int type);
BaseType_t xRingbufferSend(RingbufHandle_t ring_buffer,
                           const void *data,
                           size_t size,
                           TickType_t ticks_to_wait);
void *xRingbufferReceiveUpTo(RingbufHandle_t ring_buffer,
                             size_t *item_size,
                             TickType_t ticks_to_wait,
                             size_t max_size);
void vRingbufferReturnItem(RingbufHandle_t ring_buffer, void *item);
void vRingbufferGetInfo(RingbufHandle_t ring_buffer,
                        UBaseType_t *free,
                        UBaseType_t *read,
                        UBaseType_t *write,
                        UBaseType_t *acquire,
                        UBaseType_t *items_waiting);
void vRingbufferDelete(RingbufHandle_t ring_buffer);

#ifdef __cplusplus
}
#endif
