#ifndef FREERTOS_MODEL_H
#define FREERTOS_MODEL_H
#include <stdint.h>
#include <stddef.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t, StackType_t;
typedef struct { uint32_t notify; } StaticTask_t;
typedef StaticTask_t *TaskHandle_t;
typedef struct { unsigned cap, item_size, head, tail, count; uint8_t *bytes; } StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define configTICK_RATE_HZ 1000U
#define configASSERT(x) do { if (!(x)) model_assert(); } while (0)
#define taskSCHEDULER_NOT_STARTED 0
#define taskSCHEDULER_RUNNING 1
#define taskSCHEDULER_SUSPENDED 2
#define portYIELD_FROM_ISR(x) ((void)(x))
void model_assert(void);
#endif
