/* Gercek uart_rx/parser/frame kodu; yalniz HAL ve register'lar modellenir.
 * Her senaryo ayri surecte calisir: modul globals'ini sifirlayan test API'si yok. */
#include <stdio.h>
#include <string.h>
#include "uart_comm_internal.h"
#include "app_protocol.h"
#include "protocol_uart.h"

#ifdef UART_COMM_TEST
static app_proto_state_t read_app_state(void)
{
    app_proto_state_t state;
    (void)app_protocol_get_snapshot(&state);
    return state;
}
#endif


static protocol_uart_t rx_protocol;
static uint8_t protocol_ready;
static const frame_parser_t *test_rx_parser(void) { return &rx_protocol.parser; }
static void test_rx_set_handler(frame_handler_t handler, void *user)
{
    if (!protocol_ready) {
        protocol_uart_init(&rx_protocol, handler, user);
        protocol_ready = 1U;
    } else { rx_protocol.on_frame = handler; rx_protocol.user = user; }
    rx_set_handler(protocol_uart_on_rx, &rx_protocol);
}
uint32_t model_primask;
static uint32_t tick;
static USART_TypeDef regs;
#define stream model_dma_streams[5]
static DMA_HandleTypeDef dma = { .Instance = &stream, .State = HAL_DMA_STATE_READY };
static UART_HandleTypeDef uart = {
    .Instance = &regs, .hdmarx = &dma, .RxState = HAL_UART_STATE_READY,
    .RxEventType = HAL_UART_RXEVENT_IDLE, .gState = HAL_UART_STATE_READY
};
static uint8_t *rx_buffer;
static uint16_t rx_size;
static unsigned abort_calls;
static int abort_no_callback, abort_keeps_en, start_without_en;
static int fault_on_running_unlock, tx_irq_on_clear, tx_irq_pending;

/* Iki belirli IRQ sinirini enjekte eder; genel donanim zamanlama modeli degil. */
void model_set_primask(uint32_t value)
{
    model_primask = value;
    if (value != 0U) return;
    if (tx_irq_pending) { tx_irq_pending = 0; regs.CR1 &= ~USART_CR1_TCIE; }
    if (fault_on_running_unlock && rx_get_phase() == UART_RX_PHASE_RUNNING &&
        rx_test_recovery_active()) {
        fault_on_running_unlock = 0;
        rx_on_error(&uart, HAL_UART_ERROR_DMA);
        tick++; /* IRQ'dan sonra ana akis saglik zamanini yazarsa bir tick gecikir. */
    }
}

void model_clear_bit(volatile uint32_t *reg, uint32_t bit)
{
    uint32_t saved = *reg;
    if (tx_irq_on_clear && reg == &regs.CR1) {
        tx_irq_on_clear = 0;
        if (model_primask) tx_irq_pending = 1;
        else regs.CR1 &= ~USART_CR1_TCIE;
    }
    *reg = saved & ~bit;
}

uint32_t HAL_GetTick(void) { return tick; }
void model_bitband_clear(volatile uint32_t *reg, uint32_t bit) { *reg &= ~bit; }
HAL_StatusTypeDef HAL_UART_AbortTransmit_IT(UART_HandleTypeDef *u) { (void)u; return HAL_ERROR; }
HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef *u, const uint8_t *p, uint16_t n)
{ (void)u; (void)p; (void)n; return HAL_ERROR; }
/* RX fixture'i TX calistirmaz; ortak callback'in TX kapisi ayri suite'te sinanir. */
HAL_UART_RxEventTypeTypeDef HAL_UARTEx_GetRxEventType(UART_HandleTypeDef *u) { return u->RxEventType; }

HAL_StatusTypeDef HAL_UARTEx_ReceiveToIdle_DMA(UART_HandleTypeDef *u, uint8_t *p, uint16_t n)
{
    if (u->RxState != HAL_UART_STATE_READY) return HAL_BUSY;
    rx_buffer = p; rx_size = n;
    u->RxState = HAL_UART_STATE_BUSY_RX;
    u->ReceptionType = HAL_UART_RECEPTION_TOIDLE;
    u->ErrorCode = 0;
    u->hdmarx->Instance->NDTR = n;
    u->hdmarx->Instance->flags = 0;
    u->hdmarx->Instance->CR = start_without_en ? 0U : DMA_SxCR_EN;
    u->hdmarx->State = start_without_en ? HAL_DMA_STATE_READY : HAL_DMA_STATE_BUSY;
    u->Instance->CR3 = USART_CR3_DMAR | USART_CR3_EIE;
    u->Instance->CR1 = USART_CR1_IDLEIE;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_AbortReceive_IT(UART_HandleTypeDef *u)
{
    abort_calls++;
    u->Instance->CR3 &= ~(USART_CR3_DMAR | USART_CR3_EIE);
    u->Instance->CR1 &= ~(USART_CR1_IDLEIE | USART_CR1_RXNEIE | USART_CR1_PEIE);
    if (!abort_keeps_en) {
        u->hdmarx->Instance->CR &= ~DMA_SxCR_EN;
        u->hdmarx->State = HAL_DMA_STATE_READY;
    }
    u->RxState = HAL_UART_STATE_READY;
    u->ReceptionType = HAL_UART_RECEPTION_STANDARD;
    if (!abort_no_callback) HAL_UART_AbortReceiveCpltCallback(u);
    return HAL_OK;
}

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expr); return 1; } } while (0)

static void event(HAL_UART_RxEventTypeTypeDef type)
{
    uart.RxEventType = type;
    HAL_UARTEx_RxEventCallback(&uart, (uint16_t)(rx_size - stream.NDTR));
}

static void input(const uint8_t *bytes, unsigned n, int idle, int tc_irq)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        rx_buffer[rx_size - stream.NDTR] = bytes[i];
        if (--stream.NDTR == 0U) {
            stream.NDTR = rx_size;
            stream.flags |= 1U;
            if (tc_irq) { stream.flags &= ~1U; event(HAL_UART_RXEVENT_TC); }
        }
    }
    if (idle) event(HAL_UART_RXEVENT_IDLE);
}

static int start(void)
{
    app_protocol_init();
    test_rx_set_handler(app_protocol_on_frame, NULL);
    CHECK(rx_start(&uart) == HAL_OK);
    return 0;
}

static void drive(unsigned ms)
{
    unsigned i;
    for (i = 0; i < ms; i++) { tick++; rx_service(); }
}

static int normal(void)
{
    uint8_t f[FRAME_MAX_SIZE];
    CHECK(start() == 0);
    input(f, frame_build_joystick(f, sizeof(f), 123, -456, 1), 1, 1);
    rx_service();
    CHECK(read_app_state().frames_handled == 1);
    CHECK(read_app_state().joy_x == 123 && read_app_state().joy_y == -456);
    CHECK(rx_next_wait_ms(tick) == UINT32_MAX);
    return 0;
}

static int active_start(void)
{
    uint8_t f[FRAME_MAX_SIZE];
    uint8_t n;
    CHECK(start() == 0);
    n = frame_build_joystick(f, sizeof(f), 1, 2, 7);
    input(f, 7, 1, 1); rx_service();
    CHECK(rx_start(&uart) == HAL_BUSY);
    CHECK(test_rx_parser()->len == 7 && rx_get_consumed() == 7);
    input(f + 7, n - 7, 1, 1); rx_service();
    CHECK(read_app_state().frames_handled == 1 && read_app_state().last_seq == 7);
    return 0;
}

static int budget(void)
{
    uint8_t f[13], bytes[130];
    unsigned i;
    CHECK(start() == 0);
    CHECK(frame_build_joystick(f, sizeof(f), 1, 2, 1) == 13);
    for (i = 0; i < 10; i++) memcpy(bytes + i * 13, f, 13);
    input(bytes, sizeof(bytes), 1, 1);
    rx_service(); CHECK(rx_get_consumed() == 64);
    rx_service(); CHECK(rx_get_consumed() == 128);
    rx_service(); CHECK(rx_get_consumed() == 130);
    CHECK(read_app_state().frames_handled == 10);
    return 0;
}

static int overrun(void)
{
    uint8_t bytes[768];
    memset(bytes, 0, sizeof(bytes));
    CHECK(start() == 0);
    input(bytes, sizeof(bytes), 1, 1); rx_service();
    CHECK(rx_stats.overruns == 1 && rx_stats.discarded_bytes == 768);
    CHECK(rx_get_consumed() == 768 && read_app_state().frames_handled == 0);
    return 0;
}

static int pending_tc(void)
{
    uint8_t bytes[257] = {0};
    uint32_t before, after;
    CHECK(start() == 0);
    input(bytes, sizeof(bytes), 0, 0);
    CHECK(rx_get_produced(&before) && before == 257);
    stream.flags &= ~1U; event(HAL_UART_RXEVENT_TC);
    CHECK(rx_get_produced(&after) && after == before);
    event(HAL_UART_RXEVENT_IDLE);
    CHECK(rx_get_produced(&after) && after == before);
    CHECK(rx_producer_from(0xFFFFFF00U, 1, 256) == 0);
    return 0;
}

static int sample_retry(void)
{
    uint8_t f[13];
    CHECK(start() == 0);
    input(f, frame_build_joystick(f, sizeof(f), 3, 4, 8), 1, 1);
    rx_test_set_copy_hook(UART_RX_COPY_HOOK_SAMPLE_FAIL);
    rx_service(); CHECK(read_app_state().frames_handled == 0);
    rx_test_force_sample_fail(0);
    drive(2); CHECK(read_app_state().frames_handled == 1);
    return 0;
}

static int timeout_sample_stuck(void)
{
    const uint8_t header[] = {0xAA, 0x55, 1, 0x20, 55, 1, 0};
    CHECK(start() == 0);
    input(header, sizeof(header), 1, 1); rx_service();
    rx_test_force_sample_fail(1);
    tick = 50; rx_service();
    drive(22);
    CHECK(rx_stats.sample_fails == 1);
    CHECK(abort_calls == 1 && rx_stats.frame_timeouts == 0);
    return 0;
}

static int partial_timeout(void)
{
    const uint8_t header[] = {0xAA, 0x55, 1, 0x20, 55, 1, 0};
    CHECK(start() == 0);
    input(header, sizeof(header), 1, 1); rx_service();
    tick = 49; rx_service(); CHECK(rx_stats.frame_timeouts == 0);
    tick = 50; rx_service(); CHECK(rx_stats.frame_timeouts == 1);
    CHECK(test_rx_parser()->len == 0 && rx_next_wait_ms(tick) == UINT32_MAX);
    return 0;
}

static int continuation(void)
{
    uint8_t f[64], payload[55] = {0};
    CHECK(start() == 0);
    CHECK(frame_build(f, sizeof(f), FRAME_TYPE_SET_OUTPUT, 1, payload, 55) == 64);
    input(f, 7, 1, 1); rx_service();
    tick = 47; input(f + 7, 57, 0, 1);
    tick = 50; rx_service(); rx_service();
    CHECK(read_app_state().frames_handled == 1 && rx_stats.frame_timeouts == 0);
    return 0;
}

static int healthy_idle(void)
{
    CHECK(start() == 0);
    rx_test_inject_error(); rx_service(); drive(10);
    CHECK(rx_get_phase() == UART_RX_PHASE_RUNNING);
    CHECK(rx_test_recovery_active() == 1);
    tick += 2000;
    rx_test_inject_error(); rx_service(); drive(20);
    CHECK(rx_get_phase() == UART_RX_PHASE_RUNNING);
    CHECK(rx_stats.restarts == 2 && rx_stats.recovery_fails == 0);
    return 0;
}

static int retry_exhausted(void)
{
    CHECK(start() == 0);
    rx_force_restart_fail(1);
    rx_test_inject_error(); rx_service(); drive(40);
    CHECK(rx_get_phase() == UART_RX_PHASE_FAULT);
    CHECK(rx_stats.restart_fails == 5 && rx_is_quiescent());
    rx_force_restart_fail(0);
    CHECK(rx_request_recovery()); drive(10);
    CHECK(rx_get_phase() == UART_RX_PHASE_RUNNING);
    return 0;
}

static int missing_callback(void)
{
    CHECK(start() == 0); abort_no_callback = 1;
    rx_test_inject_error(); rx_service(); drive(10);
    CHECK(rx_get_phase() == UART_RX_PHASE_RUNNING && rx_stats.restarts == 1);
    return 0;
}

static int callback_with_en(void)
{
    CHECK(start() == 0); abort_keeps_en = 1;
    rx_test_inject_error(); rx_service(); drive(25);
    CHECK(rx_get_phase() == UART_RX_PHASE_FAULT && !rx_is_quiescent());
    CHECK(rx_start(&uart) == HAL_BUSY && rx_stats.restarts == 0);
    return 0;
}

static int stopped_irq(void)
{
    CHECK(start() == 0);
    uart.RxState = HAL_UART_STATE_READY; regs.CR3 &= ~USART_CR3_DMAR;
    dma.State = HAL_DMA_STATE_ABORT; /* EN hala set, UART error callback gecikti. */
    uart_comm_on_uart_irq_exit(&uart);
    CHECK(rx_next_wait_ms(tick) == 0);
    return 0;
}

static int unhealthy_start(void)
{
    start_without_en = 1;
    CHECK(rx_start(&uart) == HAL_ERROR);
    CHECK(rx_get_phase() == UART_RX_PHASE_FAULT);
    return 0;
}

static int overwrite_copy(void)
{
    uint8_t f[13];
    CHECK(start() == 0);
    input(f, frame_build_joystick(f, sizeof(f), 3, 4, 1), 1, 1);
    rx_test_set_copy_hook(UART_RX_COPY_HOOK_OVERWRITE);
    rx_service_budget(64);
    CHECK(read_app_state().frames_handled == 0 && rx_get_consumed() == 0);
    CHECK(rx_stats.copy_rejects == 1);
    return 0;
}

static int error_copy(void)
{
    uint8_t f[13];
    CHECK(start() == 0);
    input(f, frame_build_joystick(f, sizeof(f), 3, 4, 1), 1, 1);
    rx_test_set_copy_hook(UART_RX_COPY_HOOK_ERROR_GEN);
    rx_service_budget(64);
    CHECK(read_app_state().frames_handled == 0 && rx_get_consumed() == 0);
    return 0;
}

static int primask_preserved(void)
{
    uint32_t p;
    CHECK(start() == 0); model_primask = 1;
    CHECK(rx_get_produced(&p) && model_primask == 1);
    rx_service(); CHECK(model_primask == 1);
    return 0;
}

static int healthy_start_irq(void)
{
    CHECK(start() == 0);
    rx_test_inject_error(); rx_service();
    fault_on_running_unlock = 1;
    drive(1); /* Ilk restart yayinlanir yayinlanmaz tekrar hata. */
    CHECK(!fault_on_running_unlock);
    drive(10);
    CHECK(rx_get_phase() == UART_RX_PHASE_RUNNING);
    CHECK(rx_test_get_restart_tries() == 2);
    return 0;
}

static int fault_preserves_tx_irq(void)
{
    CHECK(start() == 0); abort_keeps_en = 1;
    rx_test_inject_error(); rx_service();
    regs.CR1 |= USART_CR1_TCIE;
    tx_irq_on_clear = 1;
    drive(25);
    CHECK(rx_get_phase() == UART_RX_PHASE_FAULT);
    CHECK(!tx_irq_on_clear && (regs.CR1 & USART_CR1_TCIE) == 0U);
    return 0;
}

static int rebind(void)
{
    UART_HandleTypeDef other = uart;
    CHECK(start() == 0);
    CHECK(rx_start(&other) == HAL_BUSY);
    CHECK(rx_get_phase() == UART_RX_PHASE_RUNNING && stream.CR == DMA_SxCR_EN);
    return 0;
}

static int sync_start_error(void)
{
    rx_test_sync_error_on_start(1);
    CHECK(rx_start(&uart) == HAL_ERROR);
    CHECK(rx_get_phase() == UART_RX_PHASE_FAULT && !rx_is_quiescent());
    CHECK(rx_start(&uart) == HAL_BUSY);
    return 0;
}

static int producer_boundaries(void)
{
    static const unsigned points[] = {0, 127, 128, 255, 256, 257, 512};
    uint8_t bytes[512] = {0};
    unsigned i, previous = 0;
    uint32_t produced;
    CHECK(start() == 0);
    for (i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        input(bytes, points[i] - previous, 1, 1);
        CHECK(rx_get_produced(&produced) && produced == points[i]);
        previous = points[i];
    }
    return 0;
}

static int full_lap_and_300(void)
{
    uint8_t bytes[300] = {0};
    CHECK(start() == 0);
    input(bytes, 256, 1, 1); rx_service();
    CHECK(rx_stats.overruns == 1 && rx_get_consumed() == 256);
    input(bytes, 300, 1, 1); rx_service();
    CHECK(rx_stats.overruns == 2 && rx_stats.discarded_bytes == 556);
    return 0;
}

static int restart_progress(void)
{
    uint8_t bytes[513] = {0};
    uint32_t produced;
    CHECK(start() == 0);
    input(bytes, 513, 1, 1); rx_service();
    rx_test_inject_error(); rx_service(); drive(10);
    input(bytes, 1, 1, 1);
    CHECK(rx_get_produced(&produced) && produced == 1);
    rx_service();
    CHECK(rx_get_consumed() == 1 && rx_stats.overruns == 1);
    return 0;
}

static int error_each_restart(void)
{
    CHECK(start() == 0);
    rx_test_sync_error_on_start(1);
    rx_test_inject_error(); rx_service(); drive(120);
    CHECK(rx_get_phase() == UART_RX_PHASE_FAULT);
    CHECK(rx_stats.restart_fails == 5 && rx_is_quiescent());
    return 0;
}

static int timeout_rearm(void)
{
    const uint8_t headers[] = {0xAA, 0x55, 1, 0x20, 55, 1, 0,
                              0xAA, 0x55, 1, 0x20, 55, 2, 0};
    CHECK(start() == 0);
    input(headers, sizeof(headers), 1, 1); rx_service();
    tick = 50; rx_service();
    CHECK(rx_stats.frame_timeouts == 1 && test_rx_parser()->len == 7);
    CHECK(rx_next_wait_ms(tick) == 50);
    tick = 100; rx_service();
    CHECK(rx_stats.frame_timeouts == 2 && test_rx_parser()->len == 0);
    return 0;
}

static int early_fault_late_service(void)
{
    CHECK(start() == 0);
    rx_test_inject_error(); rx_service(); drive(10);
    rx_test_inject_error(); tick = 200; rx_service(); drive(10);
    CHECK(rx_get_phase() == UART_RX_PHASE_FAULT);
    CHECK(rx_stats.restarts == 1); /* Gec servis erken hatayi affetmez. */
    return 0;
}

static int valid_frame_closes_recovery(void)
{
    uint8_t f[13];
    unsigned i;
    CHECK(start() == 0);
    for (i = 0; i < 6; i++) {
        rx_test_inject_error(); rx_service(); drive(8);
        CHECK(rx_get_phase() == UART_RX_PHASE_RUNNING);
        input(f, frame_build_joystick(f, sizeof(f), 1, 2, (uint16_t)i), 1, 1);
        rx_service();
        CHECK(!rx_test_recovery_active());
    }
    CHECK(read_app_state().frames_handled == 6 && rx_stats.restarts == 6);
    return 0;
}
static int cold_start_recovers(void)
{
    uint8_t f[13];
    app_protocol_init(); test_rx_set_handler(app_protocol_on_frame, NULL);
    rx_force_start_fail(1);
    CHECK(rx_start(&uart) == HAL_ERROR);
    CHECK(rx_stats.start_fails == 1);
    drive(10);
    CHECK(rx_get_phase() == UART_RX_PHASE_RUNNING && rx_stats.restarts == 1);
    input(f, frame_build_joystick(f, sizeof(f), 1, 2, 1), 1, 1);
    rx_service(); CHECK(read_app_state().frames_handled == 1);
    return 0;
}
static int app_snapshot(void)
{
    app_proto_state_t out;
    uint8_t payload[] = {0x34, 0x12, 0xFE, 0xFF};
    frame_info_t info = {FRAME_TYPE_JOYSTICK, 65535U, 4U, payload};
    app_protocol_init(); app_protocol_on_frame(&info, NULL);
    CHECK(!app_protocol_get_snapshot(NULL));
    model_primask = 1U;
    CHECK(app_protocol_get_snapshot(&out) && model_primask == 1U);
    CHECK(out.joy_x == 0x1234 && out.joy_y == -2 && out.next_seq == 0U && out.frames_handled == 1U);
    model_primask = 0U;
    CHECK(app_protocol_get_snapshot(&out) && model_primask == 0U);
    return 0;
}

static uint8_t raw_received[64];
static uint16_t raw_count;
static uint32_t raw_rx(uart_comm_rx_event_t event, const uint8_t *data, uint16_t len, void *user)
{
    (void)user;
    if (event == UART_COMM_RX_DATA) {
        memcpy(raw_received + raw_count, data, len);
        raw_count += len;
    }
    return 0U;
}
static int raw_bytes(void)
{
    const uint8_t data[] = {0, 0xAA, 0x55, 0xFF, 7};
    rx_set_handler(raw_rx, NULL);
    CHECK(rx_start(&uart) == HAL_OK);
    input(data, sizeof(data), 1, 1); rx_service();
    CHECK(raw_count == sizeof(data) && !memcmp(data, raw_received, sizeof(data)));
    CHECK(rx_next_wait_ms(tick) == UINT32_MAX);
    return 0;
}
static int reset_discards_partial(void)
{
    protocol_uart_t adapter;
    uint8_t f[13];
    app_protocol_init();
    protocol_uart_init(&adapter, app_protocol_on_frame, NULL);
    rx_set_handler(protocol_uart_on_rx, &adapter);
    CHECK(rx_start(&uart) == HAL_OK);
    frame_build_joystick(f, sizeof(f), 1, 2, 9);
    input(f, 7, 1, 1); rx_service();
    CHECK(adapter.parser.len == 7);
    adapter.parser.frames_ok = 5;
    rx_test_inject_error(); rx_service(); drive(10);
    CHECK(adapter.parser.len == 0 && adapter.parser.frames_ok == 5);
    input(f + 7, 6, 1, 1); rx_service();
    CHECK(read_app_state().frames_handled == 0);
    input(f, sizeof(f), 1, 1); rx_service();
    CHECK(read_app_state().frames_handled == 1 && adapter.parser.frames_ok == 6);
    return 0;
}
static int timeout_progress_and_wrap(void)
{
    protocol_uart_t adapter;
    uint8_t f[13];
    protocol_uart_init(&adapter, NULL, NULL);
    rx_set_handler(protocol_uart_on_rx, &adapter);
    tick = UINT32_MAX - 20U;
    CHECK(rx_start(&uart) == HAL_OK);
    frame_build_joystick(f, sizeof(f), 1, 2, 9);
    input(f, 7, 1, 1); rx_service();
    tick += 40U;
    input(f + 7, 1, 1, 1); rx_service();
    CHECK(rx_next_wait_ms(tick) == 50U);
    tick += 49U; rx_service();
    CHECK(adapter.parser.timeouts == 0 && rx_next_wait_ms(tick) == 1U);
    tick++; rx_service();
    CHECK(adapter.parser.timeouts == 1 && adapter.parser.len == 0);
    CHECK(rx_next_wait_ms(tick) == UINT32_MAX);
    return 0;
}
static void error_in_frame(const frame_info_t *info, void *user)
{
    (void)info; (void)user;
    rx_on_error(&uart, HAL_UART_ERROR_DMA);
}
static int validated_with_new_error(void)
{
    protocol_uart_t adapter;
    uint8_t f[13];
    protocol_uart_init(&adapter, error_in_frame, NULL);
    rx_set_handler(protocol_uart_on_rx, &adapter);
    CHECK(rx_start(&uart) == HAL_OK);
    rx_test_inject_error(); rx_service(); drive(10);
    CHECK(rx_test_recovery_active());
    input(f, frame_build_joystick(f, sizeof(f), 1, 2, 9), 1, 1); rx_service();
    CHECK(adapter.parser.frames_ok == 1 && rx_test_recovery_active());
    rx_service();
    CHECK(rx_get_phase() != UART_RX_PHASE_RUNNING);
    return 0;
}
int main(int argc, char **argv)
{
    static const struct { const char *name; int (*run)(void); } tests[] = {
        {"normal", normal}, {"active_start", active_start}, {"budget", budget},
        {"overrun", overrun}, {"pending_tc", pending_tc}, {"sample_retry", sample_retry},
        {"timeout_sample_stuck", timeout_sample_stuck}, {"partial_timeout", partial_timeout},
        {"continuation", continuation}, {"healthy_idle", healthy_idle},
        {"retry_exhausted", retry_exhausted}, {"missing_callback", missing_callback},
        {"callback_with_en", callback_with_en}, {"stopped_irq", stopped_irq},
        {"unhealthy_start", unhealthy_start}, {"overwrite_copy", overwrite_copy},
        {"error_copy", error_copy}, {"primask_preserved", primask_preserved},
        {"healthy_start_irq", healthy_start_irq}, {"fault_preserves_tx_irq", fault_preserves_tx_irq},
        {"rebind", rebind}, {"sync_start_error", sync_start_error},
        {"producer_boundaries", producer_boundaries}, {"full_lap_and_300", full_lap_and_300},
        {"restart_progress", restart_progress}, {"error_each_restart", error_each_restart},
        {"timeout_rearm", timeout_rearm}, {"early_fault_late_service", early_fault_late_service},
        {"valid_frame_closes_recovery", valid_frame_closes_recovery},
        {"cold_start_recovers", cold_start_recovers}, {"app_snapshot", app_snapshot},
        {"raw_bytes", raw_bytes}, {"reset_discards_partial", reset_discards_partial},
        {"timeout_progress_and_wrap", timeout_progress_and_wrap},
        {"validated_with_new_error", validated_with_new_error}
    };
    unsigned i;
    if (argc != 2) return 2;
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (strcmp(argv[1], tests[i].name) == 0) return tests[i].run();
    }
    return 2;
}
