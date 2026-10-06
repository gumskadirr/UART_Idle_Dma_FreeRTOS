#include "FreeRTOS.h"
QueueHandle_t xQueueCreateStatic(UBaseType_t cap, UBaseType_t item_size, uint8_t *bytes, StaticQueue_t *queue);
BaseType_t xQueueSendToBack(QueueHandle_t queue, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue);
