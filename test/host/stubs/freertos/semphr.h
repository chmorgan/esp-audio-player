#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

struct host_test_semaphore;

typedef struct host_test_semaphore *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks_to_wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);

#ifdef __cplusplus
}
#endif
