/* Gercek birlesik C + queue/notification modeli; donanim scheduler kaniti degil. */
#define HAL_UARTEx_ReceiveToIdle_DMA fixture_unused_rx_start
#define HAL_UART_AbortReceive_IT fixture_unused_rx_abort
#define main tx_fixture_main
#include "test_uart_tx.c"
#undef main
#undef HAL_UARTEx_ReceiveToIdle_DMA
#undef HAL_UART_AbortReceive_IT
#include "protocol_uart.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <stdlib.h>
#include <setjmp.h>

uint32_t model_ipsr;
static int scheduler, violations, notifications, isr_notifications;
static TaskHandle_t owner;
static uart_comm_tx_result_t results[32];
static unsigned result_count;
static void (*enqueue_hook)(void);
static int hook_after;
static void (*owner_fn)(void *);
static void (*wait_hook)(void);
static jmp_buf escape;
static unsigned wait_count;
static uint32_t first_wait;
static uint8_t *rx_ptr;
static int reenter, notify_step;
static frame_handler_t frame_handler;
static protocol_uart_t comm_protocol;

void model_assert(void) { abort(); }
HAL_StatusTypeDef HAL_UARTEx_ReceiveToIdle_DMA(UART_HandleTypeDef *u, uint8_t *p, uint16_t n)
{
    rx_ptr = p; u->RxState = HAL_UART_STATE_BUSY_RX; u->ReceptionType = HAL_UART_RECEPTION_TOIDLE;
    u->hdmarx->State = HAL_DMA_STATE_BUSY; u->hdmarx->Instance->CR = DMA_SxCR_EN;
    u->hdmarx->Instance->NDTR = n; u->Instance->CR3 |= USART_CR3_DMAR | USART_CR3_EIE;
    u->Instance->CR1 |= USART_CR1_IDLEIE; return HAL_OK;
}
HAL_StatusTypeDef HAL_UART_AbortReceive_IT(UART_HandleTypeDef *u)
{
    u->RxState = HAL_UART_STATE_READY; u->ReceptionType = HAL_UART_RECEPTION_STANDARD;
    u->hdmarx->State = HAL_DMA_STATE_READY; u->hdmarx->Instance->CR &= ~DMA_SxCR_EN;
    u->Instance->CR3 &= ~(USART_CR3_DMAR | USART_CR3_EIE);
    u->Instance->CR1 &= ~(USART_CR1_IDLEIE | USART_CR1_RXNEIE | USART_CR1_PEIE);
    HAL_UART_AbortReceiveCpltCallback(u); return HAL_OK;
}
static void check_api(void) { if (model_primask) violations++; }
TaskHandle_t xTaskCreateStatic(void (*fn)(void *), const char *name, uint32_t depth,
                              void *arg, UBaseType_t priority, StackType_t *stack, StaticTask_t *tcb)
{ owner_fn = fn; (void)name; (void)arg; (void)priority; (void)stack; if (depth != 512) violations++; return owner = tcb; }
BaseType_t xTaskGetSchedulerState(void) { return scheduler; }
BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value, eNotifyAction action)
{ check_api(); (void)action; notifications++; task->notify |= value;
  if (notify_step) { notify_step = 0; comm_test_service_once(); }
  return pdPASS; }
BaseType_t xTaskNotifyFromISR(TaskHandle_t task, uint32_t value, eNotifyAction action, BaseType_t *woken)
{ check_api(); (void)action; isr_notifications++; task->notify |= value; *woken = pdFALSE; return pdPASS; }
BaseType_t xTaskNotifyWait(uint32_t entry, uint32_t clear, uint32_t *events, TickType_t wait)
{ check_api(); if (entry) violations++;
  if (wait_count++ == 0U) { first_wait = wait; if (wait_hook) wait_hook(); }
  else longjmp(escape, 1);
  *events = owner->notify; owner->notify &= ~clear;
  return *events ? pdTRUE : pdFALSE; }
QueueHandle_t xQueueCreateStatic(UBaseType_t cap, UBaseType_t item, uint8_t *bytes, StaticQueue_t *q)
{ q->cap = cap; q->item_size = item; q->bytes = bytes; return q; }
BaseType_t xQueueSendToBack(QueueHandle_t q, const void *item, TickType_t wait)
{
    void (*hook)(void) = enqueue_hook;
    check_api(); if (wait) violations++;
    enqueue_hook = NULL;
    if (hook && !hook_after) hook();
    if (q->count == q->cap) return pdFALSE;
    memcpy(q->bytes + q->tail * q->item_size, item, q->item_size);
    q->tail = (q->tail + 1) % q->cap; q->count++;
    if (hook && hook_after) hook();
    return pdPASS;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    check_api(); if (wait) violations++;
    if (!q->count) return pdFALSE;
    memcpy(item, q->bytes + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->cap; q->count--; return pdPASS;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) { check_api(); return q->count; }
static void on_result(const uart_comm_tx_result_t *out, void *user)
{ (void)user; results[result_count++] = *out;
  if (reenter) { reenter = 0; (void)uart_comm_send_copy(bytes, 3U, 123U); } }
static int setup(void)
{
    uart_comm_handlers_t handlers = {.on_rx = protocol_uart_on_rx, .on_tx_result = on_result,
                                     .rx_user = &comm_protocol};
    protocol_uart_init(&comm_protocol, frame_handler, NULL);
    dma_rx.Parent = dma_tx.Parent = &uart;
    dma_rx.Init.MemInc = dma_tx.Init.MemInc = DMA_MINC_ENABLE;
    dma_rx.Init.Direction = DMA_PERIPH_TO_MEMORY; dma_rx.Init.Mode = DMA_CIRCULAR;
    dma_tx.Init.Direction = DMA_MEMORY_TO_PERIPH; dma_tx.Init.Mode = DMA_NORMAL;
    CHECK(uart_comm_init(&uart, &handlers) == HAL_OK);
    CHECK(uart_comm_init(&uart, &handlers) == HAL_BUSY);
    scheduler = taskSCHEDULER_RUNNING; comm_test_start_owner(); return 0;
}
static int wrong_dma(void)
{
    uart_comm_handlers_t handlers = {.on_tx_result = on_result};
    dma_rx.Init.MemInc = dma_tx.Init.MemInc = DMA_MINC_ENABLE;
    CHECK(uart_comm_init(&uart, &handlers) == HAL_ERROR); /* Parent eksik */
    dma_rx.Parent = dma_tx.Parent = &uart;
    CHECK(uart_comm_init(&uart, &handlers) == HAL_ERROR); /* RX circular degil */
    dma_rx.Init.Mode = DMA_CIRCULAR;
    CHECK(uart_comm_init(&uart, &handlers) == HAL_ERROR); /* TX yonu yanlis */
    dma_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
    CHECK(uart_comm_init(&uart, &handlers) == HAL_OK);
    return 0;
}
static void step(void) { comm_test_service_once(); }
static void finish_one(void) { complete(); step(); }
static int fifo(void)
{
    uart_comm_snapshot_t snap;
    unsigned i;
    CHECK(setup() == 0);
    CHECK(uart_comm_send_copy(bytes, 3, 0) == UART_COMM_ACCEPTED); step();
    for (i = 1; i <= 8; i++) CHECK(uart_comm_send_copy(bytes, 3, i) == UART_COMM_ACCEPTED);
    CHECK(uart_comm_send_copy(bytes, 3, 9) == UART_COMM_QUEUE_FULL);
    CHECK(comm_test_next_wait() == 20); /* Queue backlog SENDING'de spin sebebi degil. */
    for (i = 0; i < 9; i++) finish_one();
    CHECK(result_count == 9);
    for (i = 0; i < 9; i++) CHECK(results[i].tag == i && results[i].code == UART_COMM_TX_COMPLETE);
    CHECK(uart_comm_get_snapshot(&snap) && snap.tx_accepted == 9 && snap.tx_completed == 9);
    CHECK(snap.tx_queue_full == 1 && snap.tx_queue_depth == 0 && !violations);
    CHECK(comm_test_next_wait() == UINT32_MAX); return 0;
}
static int validation(void)
{
    uart_comm_snapshot_t snap;
    CHECK(uart_comm_get_snapshot(&snap) && !snap.initialized);
    CHECK(!uart_comm_get_snapshot(NULL)); CHECK(setup() == 0);
    CHECK(uart_comm_send_copy(bytes, 0, 1) == UART_COMM_INVALID);
    CHECK(uart_comm_send_copy(bytes, 256, 1) == UART_COMM_INVALID);
    CHECK(uart_comm_send_copy(NULL, 1, 1) == UART_COMM_INVALID);
    model_ipsr = 1; CHECK(uart_comm_send_copy(bytes, 1, 1) == UART_COMM_INVALID);
    CHECK(!uart_comm_request_recovery(UART_COMM_RECOVER_RX)); model_ipsr = 0;
    CHECK(!uart_comm_request_recovery(0) && !uart_comm_request_recovery(4));
    CHECK(!result_count && !violations); return 0;
}
static void fault_hook(void) { comm_test_force_fault(); step(); }
static void recover_hook(void)
{ fault_hook(); (void)uart_comm_request_recovery(UART_COMM_RECOVER_TX); step(); }
static int epoch(int recover)
{
    CHECK(setup() == 0); enqueue_hook = recover ? recover_hook : fault_hook;
    CHECK(uart_comm_send_copy(bytes, 3, 44) == UART_COMM_ACCEPTED); step();
    CHECK(result_count == 1 && results[0].tag == 44 && results[0].code == UART_COMM_TX_CANCELLED_FAULT);
    CHECK(uart_comm_send_copy(bytes, 3, 45) == (recover ? UART_COMM_ACCEPTED : UART_COMM_NOT_READY));
    CHECK(!violations); return 0;
}
static void result_before_return(void) { step(); finish_one(); }
static int early_result(void)
{
    CHECK(setup() == 0); hook_after = 1; enqueue_hook = result_before_return;
    CHECK(uart_comm_send_copy(bytes, 3, 55) == UART_COMM_ACCEPTED);
    CHECK(result_count == 1 && results[0].tag == 55); step();
    CHECK(!violations); return 0;
}
static int notify_context(void)
{
    CHECK(setup() == 0); notifications = isr_notifications = 0;
    model_ipsr = 1; HAL_UART_TxCpltCallback(&uart); model_ipsr = 0;
    CHECK(isr_notifications == 1); HAL_UART_AbortReceiveCpltCallback(&uart);
    CHECK(notifications == 1 && !violations); return 0;
}
static int callback_reenqueue(void)
{
    CHECK(setup() == 0); reenter = 1;
    CHECK(uart_comm_send_copy(bytes, 3, 122U) == UART_COMM_ACCEPTED); step(); finish_one(); finish_one();
    CHECK(result_count == 2 && results[0].tag == 122 && results[1].tag == 123);
    CHECK(!violations); return 0;
}
static int task_wait_race(void)
{
    CHECK(setup() == 0);
    CHECK(uart_comm_send_copy(bytes, 3, 77U) == UART_COMM_ACCEPTED);
    wait_hook = complete; /* Callback tam service/sleep gecisinde gelir. */
    if (!setjmp(escape)) owner_fn(NULL);
    CHECK(first_wait == 20U && result_count == 1 && results[0].tag == 77U);
    CHECK(!violations); return 0;
}
static int combined_error(void)
{
    uart_comm_snapshot_t snap;
    CHECK(setup() == 0);
    CHECK(uart_comm_send_copy(bytes, 3, 80U) == UART_COMM_ACCEPTED); step();
    /* Thread callback notify ile aninda preempt edilsin: RX ve TX hata
     * kayitlari notify'dan once birlikte gorunmek zorunda. */
    notify_step = 1; uart.ErrorCode = HAL_UART_ERROR_DMA; HAL_UART_ErrorCallback(&uart);
    CHECK(tx_get_state() == UART_TX_ABORTING);
    tick++; step(); CHECK(result_count == 1 && results[0].code == UART_COMM_TX_DMA_ERROR);
    CHECK(uart_comm_get_snapshot(&snap) && snap.tx_failed == 1 && snap.last_rx_error == HAL_UART_ERROR_DMA);
    CHECK(!violations); return 0;
}
static void slow_frame(const frame_info_t *info, void *user)
{ (void)info; (void)user; tick += 40U; }
static int fresh_wait(void)
{
    uint8_t frame[13];
    frame_handler = slow_frame; CHECK(setup() == 0);
    CHECK(frame_build_joystick(frame, sizeof(frame), 1, 2, 1) == 13U);
    memcpy(rx_ptr, frame, 13U); memcpy(rx_ptr + 13U, frame, 5U);
    rx_stream.NDTR = 256U - 18U; uart.RxEventType = HAL_UART_RXEVENT_IDLE;
    HAL_UARTEx_RxEventCallback(&uart, 18U); step();
    CHECK(tick == 40U && comm_test_next_wait() == 10U);
    tick = 50U; step(); CHECK(comm_test_next_wait() == UINT32_MAX);
    CHECK(!violations); return 0;
}
static int backlog_abort(void)
{
    unsigned i;
    CHECK(setup() == 0); CHECK(uart_comm_send_copy(bytes, 3, 1) == UART_COMM_ACCEPTED); step();
    for (i=2; i<10; i++) CHECK(uart_comm_send_copy(bytes, 3, i) == UART_COMM_ACCEPTED);
    abort_keeps_en = abort_no_callback = 1; tick = 20U; step();
    CHECK(tx_get_state() == UART_TX_ABORTING && comm_test_next_wait() == 1U);
    tick = 40U; step();
    for (i=0; i<8; i++) step();
    CHECK(result_count == 9 && results[0].code == UART_COMM_TX_TIMEOUT && results[0].recovery_fault);
    for (i=1; i<9; i++) CHECK(results[i].code == UART_COMM_TX_CANCELLED_FAULT);
    CHECK(comm_test_next_wait() == UINT32_MAX && !violations); return 0;
}
static int counter_wrap(void)
{
    CHECK(setup() == 0); tick = UINT32_MAX - 9U;
    CHECK(uart_comm_send_copy(bytes, 3, 90) == UART_COMM_ACCEPTED); step();
    tick = 4U; CHECK(comm_test_next_wait() == 6U);
    tick = 10U; step(); tick++; step();
    CHECK(result_count == 1 && results[0].code == UART_COMM_TX_TIMEOUT);
    CHECK(!violations); return 0;
}
static int suspended_notify(void)
{
    CHECK(setup() == 0); isr_notifications = 0; owner->notify = 0;
    scheduler = taskSCHEDULER_SUSPENDED; model_ipsr = 1U;
    uart.RxEventType = HAL_UART_RXEVENT_IDLE; HAL_UARTEx_RxEventCallback(&uart, 1U);
    CHECK(isr_notifications == 1 && owner->notify != 0U);
    scheduler = taskSCHEDULER_RUNNING; model_ipsr = 0U;
    CHECK(!violations); return 0;
}
static int deferred_recovery(unsigned count)
{
    uart_comm_snapshot_t snap;
    unsigned i;
    CHECK(setup() == 0);
    for (i=0; i<count; i++) CHECK(uart_comm_send_copy(bytes, 3, 110U+i) == UART_COMM_ACCEPTED);
    comm_test_force_fault(); CHECK(uart_comm_request_recovery(UART_COMM_RECOVER_TX));
    owner->notify = 0U; /* Waking notification gercek wait'te tuketildi. */
    for (i=0; i<count; i++) step();
    CHECK(result_count == count && comm_test_next_wait() == 0U);
    step(); CHECK(uart_comm_get_snapshot(&snap) && snap.tx_accepting && snap.tx_state == UART_TX_IDLE);
    CHECK(comm_test_next_wait() == UINT32_MAX && !violations); return 0;
}
static int foreign_uart_events(void)
{
    UART_HandleTypeDef foreign = uart;
    uart_comm_snapshot_t before, after;
    uint32_t rx_before, tx_before, wait;
    int notify_before;
    CHECK(setup() == 0);
    uart_comm_get_snapshot(&before);
    rx_before = rx_stats.rx_events; tx_before = tx_stats.tx_complete_events;
    notify_before = notifications + isr_notifications;
    wait = comm_test_next_wait(); foreign.ErrorCode = HAL_UART_ERROR_DMA;
    uart_comm_on_rx_event(&foreign, 17U); uart_comm_on_error(&foreign);
    uart_comm_on_rx_abort_complete(&foreign); uart_comm_on_tx_complete(&foreign);
    uart_comm_on_tx_abort_complete(&foreign); uart_comm_on_uart_irq_exit(&foreign);
    uart_comm_on_rx_event(NULL, 17U); uart_comm_on_error(NULL);
    uart_comm_on_rx_abort_complete(NULL); uart_comm_on_tx_complete(NULL);
    uart_comm_on_tx_abort_complete(NULL); uart_comm_on_uart_irq_exit(NULL);
    uart_comm_get_snapshot(&after);
    CHECK(!memcmp(&before, &after, sizeof(before)));
    CHECK(rx_stats.rx_events == rx_before && tx_stats.tx_complete_events == tx_before);
    CHECK(notifications + isr_notifications == notify_before && comm_test_next_wait() == wait);
    return 0;
}
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "fifo")) return fifo();
    if (!strcmp(argv[1], "wrong_dma")) return wrong_dma();
    if (!strcmp(argv[1], "validation")) return validation();
    if (!strcmp(argv[1], "epoch_fault")) return epoch(0);
    if (!strcmp(argv[1], "epoch_recovery")) return epoch(1);
    if (!strcmp(argv[1], "early_result")) return early_result();
    if (!strcmp(argv[1], "notify_context")) return notify_context();
    if (!strcmp(argv[1], "callback_reenqueue")) return callback_reenqueue();
    if (!strcmp(argv[1], "task_wait_race")) return task_wait_race();
    if (!strcmp(argv[1], "combined_error")) return combined_error();
    if (!strcmp(argv[1], "fresh_wait")) return fresh_wait();
    if (!strcmp(argv[1], "backlog_abort")) return backlog_abort();
    if (!strcmp(argv[1], "counter_wrap")) return counter_wrap();
    if (!strcmp(argv[1], "suspended_notify")) return suspended_notify();
    if (!strcmp(argv[1], "deferred_recovery_one")) return deferred_recovery(1U);
    if (!strcmp(argv[1], "deferred_recovery_eight")) return deferred_recovery(8U);
    if (!strcmp(argv[1], "foreign_uart_events")) return foreign_uart_events();
    return 2;
}
