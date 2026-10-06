#include "FreeRTOS.h"
typedef enum { eSetBits } eNotifyAction;
TaskHandle_t xTaskCreateStatic(void (*fn)(void *), const char *name, uint32_t stack_size,
                              void *arg, UBaseType_t priority, StackType_t *stack, StaticTask_t *tcb);
BaseType_t xTaskGetSchedulerState(void);
BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value, eNotifyAction action);
BaseType_t xTaskNotifyFromISR(TaskHandle_t task, uint32_t value, eNotifyAction action, BaseType_t *woken);
BaseType_t xTaskNotifyWait(uint32_t clear_entry, uint32_t clear_exit, uint32_t *events, TickType_t wait);
