#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;

#define pdFALSE 0
#define pdTRUE 1
#define pdFAIL 0
#define pdPASS 1

#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(milliseconds) ((TickType_t)(milliseconds))

#define tskIDLE_PRIORITY 0
#define tskNO_AFFINITY (-1)

#ifdef __cplusplus
}
#endif
