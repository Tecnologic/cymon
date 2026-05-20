#pragma once
// Minimal FreeRTOS.h stub for Linux host builds.
#include <stdint.h>

typedef uint32_t TickType_t;
typedef void* TaskHandle_t;
#define portMAX_DELAY   ((TickType_t)0xFFFFFFFFUL)
#define pdTRUE  1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)((ms)))
