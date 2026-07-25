#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

struct host_test_task;

typedef struct host_test_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task_function,
                                  const char *name,
                                  uint32_t stack_depth,
                                  void *argument,
                                  UBaseType_t priority,
                                  TaskHandle_t *created_task,
                                  BaseType_t core_id);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);

#ifdef __cplusplus
}
#endif
