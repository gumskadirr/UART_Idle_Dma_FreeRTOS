#include <stdio.h>
#include <string.h>
#include "uart_comm_internal.h"
#include "protocol.h"

uint32_t model_primask;
static uint32_t tick;
static USART_TypeDef regs = {.SR = USART_SR_TC};
static DMA_Stream_TypeDef rx_stream, tx_stream;
static DMA_HandleTypeDef dma_rx = { .Instance = &rx_stream, .State = HAL_DMA_STATE_READY };
static DMA_HandleTypeDef dma_tx = { .Instance = &tx_stream, .State = HAL_DMA_STATE_READY };
static UART_HandleTypeDef uart = {
    .Instance = &regs, .hdmarx = &dma_rx, .RxState = HAL_UART_STATE_READY,
    .hdmatx = &dma_tx, .gState = HAL_UART_STATE_READY
};
static int inject_on_unlock;
static int ht_clear_race;
static HAL_StatusTypeDef start_status = HAL_OK;
static int active_start_error, complete_on_start, error_on_start;
static int abort_keeps_en, abort_no_callback, abort_keeps_tc, complete_on_tick;
static unsigned abort_calls;
static const uint8_t *sent_buffer;
static void complete(void);
static const uint8_t bytes[] = {1, 2, 3};

uint32_t HAL_GetTick(void)
{
    if (complete_on_tick) { complete_on_tick = 0; complete(); }
    return tick;
}
void model_clear_bit(volatile uint32_t *reg, uint32_t bit)
{
    uint32_t old = *reg;
    if (ht_clear_race && reg == &tx_stream.CR && bit == DMA_IT_HT) {
        ht_clear_race = 0;
        complete(); /* HAL/EN degisikligi CR okuma-yazma arasinda */
    }
    *reg = old & ~bit;
}
void model_bitband_clear(volatile uint32_t *reg, uint32_t bit)
{
    if (ht_clear_race && reg == &tx_stream.CR && bit == DMA_IT_HT) {
        ht_clear_race = 0;
        complete();
    }
    *reg &= ~bit; /* Tek bit yazma: diger bitlere eski deger yazilmaz. */
}
void model_set_primask(uint32_t value)
{
    model_primask = value;
    if (!value && inject_on_unlock) {
        inject_on_unlock = 0;
        tx_on_error(&uart, HAL_UART_ERROR_DMA);
    }
}
HAL_UART_RxEventTypeTypeDef HAL_UARTEx_GetRxEventType(UART_HandleTypeDef *u) { return u->RxEventType; }
HAL_StatusTypeDef HAL_UARTEx_ReceiveToIdle_DMA(UART_HandleTypeDef *u, uint8_t *p, uint16_t n)
{ (void)u; (void)p; (void)n; return HAL_ERROR; }
HAL_StatusTypeDef HAL_UART_AbortReceive_IT(UART_HandleTypeDef *u) { (void)u; return HAL_ERROR; }
HAL_StatusTypeDef HAL_UART_AbortTransmit_IT(UART_HandleTypeDef *u)
{
    abort_calls++;
    u->Instance->CR1 &= ~(USART_CR1_TXEIE | USART_CR1_TCIE);
    u->Instance->CR3 &= ~USART_CR3_DMAT;
    u->gState = HAL_UART_STATE_READY;
    if (!abort_keeps_en) {
        u->hdmatx->State = HAL_DMA_STATE_READY;
        u->hdmatx->Instance->CR &= ~DMA_SxCR_EN;
    }
    if (!abort_keeps_tc) u->Instance->SR |= USART_SR_TC;
    if (!abort_no_callback) HAL_UART_AbortTransmitCpltCallback(u);
    return HAL_OK;
}
HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef *u, const uint8_t *p, uint16_t n)
{
    if (start_status != HAL_OK && !active_start_error) return start_status;
    sent_buffer = p;
    u->gState = HAL_UART_STATE_BUSY_TX;
    u->hdmatx->State = HAL_DMA_STATE_BUSY;
    u->hdmatx->Instance->CR |= DMA_SxCR_EN;
    u->hdmatx->Instance->NDTR = n;
    u->Instance->CR3 |= USART_CR3_DMAT;
    u->Instance->SR &= ~USART_SR_TC;
    if (complete_on_start) complete();
    if (error_on_start) tx_on_error(u, HAL_UART_ERROR_DMA);
    return start_status;
}

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expr); return 1; } } while (0)
static int start(void)
{
    CHECK(tx_init(&uart) == HAL_OK);
    CHECK(tx_send_copy(bytes, sizeof(bytes)) == UART_TX_OK);
    return 0;
}
static void complete(void)
{
    uart.gState = HAL_UART_STATE_READY;
    dma_tx.State = HAL_DMA_STATE_READY;
    tx_stream.CR &= ~DMA_SxCR_EN;
    regs.CR3 &= ~USART_CR3_DMAT;
    regs.CR1 &= ~(USART_CR1_TXEIE | USART_CR1_TCIE);
    regs.SR |= USART_SR_TC;
    HAL_UART_TxCpltCallback(&uart);
}
static int dma_error_reported(void)
{
    CHECK(start() == 0);
    uart.gState = HAL_UART_STATE_READY; /* Native HAL callback'ten once degistirir. */
    uart.ErrorCode = HAL_UART_ERROR_DMA;
    HAL_UART_ErrorCallback(&uart); tx_service(); tx_service();
    CHECK(tx_get_state() == UART_TX_IDLE && tx_stats.transfer_errors == 1);
    CHECK(tx_stats.last_hal_error == HAL_UART_ERROR_DMA);
    return 0;
}
static int line_preserves_tx(void)
{
    CHECK(start() == 0);
    uart.ErrorCode = HAL_UART_ERROR_FE;
    HAL_UART_ErrorCallback(&uart); tx_service();
    CHECK(tx_get_state() == UART_TX_SENDING && tx_stats.transfer_errors == 0);
    complete(); tx_service();
    CHECK(tx_stats.frames_sent == 1);
    return 0;
}
static int done_and_error(void)
{
    CHECK(start() == 0); complete();
    tx_on_error(&uart, HAL_UART_ERROR_DMA); tx_service(); tx_service();
    CHECK(tx_stats.frames_sent == 0 && tx_stats.transfer_errors == 1);
    CHECK(tx_get_state() == UART_TX_IDLE);
    return 0;
}
static int idle_error_ignored(void)
{
    CHECK(tx_init(&uart) == HAL_OK);
    tx_on_error(&uart, HAL_UART_ERROR_DMA);
    CHECK(tx_send_copy(bytes, sizeof(bytes)) == UART_TX_OK);
    complete(); tx_service();
    CHECK(tx_stats.frames_sent == 1 && tx_stats.transfer_errors == 0);
    return 0;
}
static int event_during_take(void)
{
    CHECK(start() == 0); inject_on_unlock = 1;
    tx_service(); tx_service();
    CHECK(!inject_on_unlock && tx_stats.transfer_errors == 1);
    CHECK(tx_stats.frames_sent == 0);
    return 0;
}

static int start_busy(void)
{
    CHECK(tx_init(&uart) == HAL_OK); start_status = HAL_BUSY;
    CHECK(tx_send_copy(bytes, sizeof(bytes)) == UART_TX_START_BUSY);
    CHECK(tx_get_state() == UART_TX_IDLE && tx_stats.start_fails == 1);
    return 0;
}
static int start_error_safe(void)
{
    CHECK(tx_init(&uart) == HAL_OK); start_status = HAL_ERROR;
    CHECK(tx_send_copy(bytes, sizeof(bytes)) == UART_TX_START_ERROR);
    CHECK(tx_get_state() == UART_TX_IDLE);
    return 0;
}
static int start_error_active(void)
{
    tx_result_t result;
    CHECK(tx_init(&uart) == HAL_OK); start_status = HAL_ERROR; active_start_error = 1;
    abort_keeps_en = 1;
    CHECK(tx_send_copy(bytes, sizeof(bytes)) == UART_TX_START_ERROR);
    CHECK(tx_get_state() == UART_TX_ABORTING);
    CHECK(tx_init(&uart) == HAL_BUSY);
    tick = 20; tx_service();
    CHECK(tx_get_state() == UART_TX_FAULT);
    CHECK(tx_init(&uart) == HAL_BUSY); /* Bekleyen sonuc ezilmez. */
    CHECK(tx_take_result(&result) && result.recovery_fault);
    CHECK(tx_init(&uart) == HAL_ERROR);
    CHECK(tx_send_copy(bytes, sizeof(bytes)) == UART_TX_NOT_READY);
    return 0;
}
static int invalid_length(void)
{
    volatile uint16_t large = 257;
    CHECK(tx_init(&uart) == HAL_OK);
    CHECK(tx_send_copy(bytes, large) == UART_TX_INVALID);
    CHECK(tx_send_copy(bytes, 65) == UART_TX_INVALID);
    CHECK(tx_send_copy(bytes, 256) == UART_TX_INVALID);
    CHECK(tx_send_copy(bytes, 0) == UART_TX_INVALID);
    CHECK(tx_send_copy(NULL, 1) == UART_TX_INVALID);
    return 0;
}
static int source_copy(void)
{
    uint8_t local[] = {7, 8, 9};
    CHECK(tx_init(&uart) == HAL_OK);
    CHECK(tx_send_copy(local, sizeof(local)) == UART_TX_OK);
    memset(local, 0, sizeof(local));
    CHECK(sent_buffer[0] == 7 && sent_buffer[1] == 8 && sent_buffer[2] == 9);
    CHECK(tx_send_copy(bytes, sizeof(bytes)) == UART_TX_BUSY);
    CHECK(tx_init(&uart) == HAL_BUSY && sent_buffer[0] == 7);
    return 0;
}
static int complete_before_return(void)
{
    complete_on_start = 1; CHECK(start() == 0);
    tx_service();
    CHECK(tx_get_state() == UART_TX_IDLE && tx_stats.frames_sent == 1);
    return 0;
}
static int error_during_start(void)
{
    error_on_start = 1; CHECK(start() == 0);
    tx_service(); tx_service();
    CHECK(tx_get_state() == UART_TX_IDLE && tx_stats.transfer_errors == 1);
    return 0;
}

static int timeout_and_reuse(void)
{
    tx_result_t result;
    CHECK(start() == 0); tick = 19; tx_service();
    CHECK(tx_get_state() == UART_TX_SENDING && tx_next_wait_ms(tick) == 1);
    tick = 20; tx_service();
    CHECK(tx_get_state() == UART_TX_ABORTING && abort_calls == 1);
    tx_service();
    CHECK(tx_get_state() == UART_TX_IDLE && tx_stats.frames_sent == 0);
    CHECK(tx_take_result(&result) && result.code == UART_TX_RESULT_TIMEOUT && !result.recovery_fault);
    CHECK(tx_send_copy(bytes, sizeof(bytes)) == UART_TX_OK);
    CHECK(abort_calls == 1);
    return 0;
}
static int result_blocks_send(void)
{
    tx_result_t result;
    CHECK(start() == 0); complete(); tx_service();
    CHECK(!tx_take_result(NULL));
    CHECK(tx_send_copy(bytes, sizeof(bytes)) == UART_TX_BUSY);
    CHECK(tx_take_result(&result) && result.code == UART_TX_RESULT_COMPLETE);
    CHECK(!tx_take_result(&result));
    CHECK(tx_send_copy(bytes, sizeof(bytes)) == UART_TX_OK);
    return 0;
}
static int missing_abort_callback(void)
{
    tx_result_t result;
    CHECK(start() == 0); abort_no_callback = 1;
    tx_on_error(&uart, HAL_UART_ERROR_DMA); tx_service(); tx_service();
    CHECK(tx_get_state() == UART_TX_IDLE);
    CHECK(tx_take_result(&result) && result.code == UART_TX_RESULT_DMA_ERROR);
    CHECK(tx_stats.abort_complete_events == 0 && !result.recovery_fault);
    return 0;
}
static int abort_callback_with_en(void)
{
    tx_result_t result;
    CHECK(start() == 0); abort_keeps_en = 1;
    tx_on_error(&uart, HAL_UART_ERROR_DMA); tx_service();
    CHECK(tx_get_state() == UART_TX_ABORTING && tx_stats.abort_complete_events == 1);
    tick = 19; tx_service(); CHECK(!tx_take_result(&result));
    tick = 20; tx_service();
    CHECK(tx_get_state() == UART_TX_FAULT);
    CHECK(tx_take_result(&result) && result.code == UART_TX_RESULT_DMA_ERROR && result.recovery_fault);
    CHECK(tx_stats.last_fail == UART_TX_FAIL_TRANSFER && tx_stats.frames_sent == 0);
    return 0;
}
static int line_not_quiet(void)
{
    tx_result_t result;
    CHECK(start() == 0); abort_keeps_tc = 1;
    tick = 20; tx_service(); tx_service();
    CHECK(tx_get_state() == UART_TX_ABORTING && !tx_take_result(&result));
    regs.SR |= USART_SR_TC; tx_service();
    CHECK(tx_get_state() == UART_TX_IDLE && tx_take_result(&result));
    return 0;
}
static int late_done_abort_and_fault(void)
{
    tx_result_t result;
    CHECK(start() == 0); abort_keeps_en = 1; tick = 20; tx_service();
    HAL_UART_TxCpltCallback(&uart); tx_service();
    CHECK(tx_get_state() == UART_TX_ABORTING && tx_stats.frames_sent == 0);
    tick = 40; tx_service();
    CHECK(tx_get_state() == UART_TX_FAULT && tx_take_result(&result));
    HAL_UART_TxCpltCallback(&uart); tx_service();
    CHECK(tx_get_state() == UART_TX_FAULT && tx_stats.frames_sent == 0);
    CHECK(tx_stats.late_completions == 2 && !tx_take_result(&result));
    return 0;
}
static int completed_before_late_service(void)
{
    tx_result_t result;
    CHECK(start() == 0); complete(); tick = 200; tx_service();
    CHECK(tx_stats.frames_sent == 1 && abort_calls == 0);
    CHECK(tx_take_result(&result) && result.code == UART_TX_RESULT_COMPLETE);
    return 0;
}
static int done_during_timeout_decision(void)
{
    CHECK(start() == 0); complete_on_tick = 1; tick = 20; tx_service();
    CHECK(tx_stats.frames_sent == 1 && abort_calls == 0);
    return 0;
}
static int tick_wrap(void)
{
    tx_result_t result;
    tick = 0xFFFFFFF5U; CHECK(start() == 0);
    tick = 8; tx_service(); CHECK(tx_get_state() == UART_TX_SENDING);
    tick = 9; tx_service(); tx_service();
    CHECK(tx_take_result(&result) && result.code == UART_TX_RESULT_TIMEOUT);
    return 0;
}
static int start_failure_result(void)
{
    tx_result_t result;
    CHECK(tx_init(&uart) == HAL_OK); start_status = HAL_BUSY;
    CHECK(tx_send_copy(bytes, sizeof(bytes)) == UART_TX_START_BUSY);
    CHECK(tx_take_result(&result) && result.code == UART_TX_RESULT_START_BUSY);
    CHECK(!tx_take_result(&result));
    return 0;
}
static int ht_clear_completion_race(void)
{
    tx_result_t out;
    ht_clear_race = 1;
    CHECK(start() == 0);
    CHECK(!ht_clear_race && (tx_stream.CR & DMA_SxCR_EN) == 0U);
    tx_service();
    CHECK(tx_take_result(&out) && out.code == UART_TX_RESULT_COMPLETE);
    return 0;
}
int main(int argc, char **argv)
{
    static const struct { const char *name; int (*run)(void); } cases[] = {
        {"dma_error_reported", dma_error_reported}, {"line_preserves_tx", line_preserves_tx},
        {"done_and_error", done_and_error}, {"idle_error_ignored", idle_error_ignored},
        {"event_during_take", event_during_take}, {"start_busy", start_busy},
        {"start_error_safe", start_error_safe}, {"start_error_active", start_error_active},
        {"invalid_length", invalid_length}, {"source_copy", source_copy},
        {"complete_before_return", complete_before_return}, {"error_during_start", error_during_start},
        {"timeout_and_reuse", timeout_and_reuse}, {"result_blocks_send", result_blocks_send},
        {"missing_abort_callback", missing_abort_callback}, {"abort_callback_with_en", abort_callback_with_en},
        {"line_not_quiet", line_not_quiet}, {"late_done_abort_and_fault", late_done_abort_and_fault},
        {"completed_before_late_service", completed_before_late_service},
        {"done_during_timeout_decision", done_during_timeout_decision},
        {"tick_wrap", tick_wrap}, {"start_failure_result", start_failure_result},
        {"ht_clear_completion_race", ht_clear_completion_race}
    };
    unsigned i;
    if (argc != 2) return 2;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        if (!strcmp(argv[1], cases[i].name)) return cases[i].run();
    return 2;
}
