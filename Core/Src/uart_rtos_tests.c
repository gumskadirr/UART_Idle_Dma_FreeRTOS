#ifdef UART_COMM_TEST
#include "uart_rtos_tests.h"
#include "uart_comm_test.h"
#include "uart_comm_tests.h"
#include "app_protocol.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <string.h>
extern UART_HandleTypeDef huart2;
extern uint32_t app_protocol_test_max_critical_cycles;

/* Kabul deneyleri uygulama taskindan public API ile calisir.
 * Servis/HAL baslatma kapilari cagirilmaz; kontrollu hata kapilari test-only. */
static StaticTask_t test_tcb, producer_tcb;
static StackType_t test_stack[1024], producer_stack[256];
static TaskHandle_t test_task;
static volatile uint32_t results, completes, failures, cancels, frames;
static volatile uint32_t tags[32], last_tag;
static volatile uart_comm_tx_code_t last_code;
static volatile uint8_t reply_enabled;
static volatile uint32_t replies_accepted, producer_accepted, producer_done;
static volatile int16_t seen_x;
static uint16_t ids[22];
static uint32_t producer_results[2];
static uint8_t producer_order_ok = 1U;
comm_test_profile_t rtos_profile;
uint32_t rtos_idle_iterations, rtos_idle_cycles, rtos_100hz_cpu_permille;
uint32_t rtos_stream_cpu_permille, rtos_100hz_frames, rtos_stream_frames;

static void on_frame(const frame_info_t *info, void *user)
{
    uint8_t reply[FRAME_MAX_SIZE], payload[2] = {info->type, 0U}, len;
    app_protocol_on_frame(info, user);
    if (info->type == FRAME_TYPE_JOYSTICK && info->payload_len == 4U)
        seen_x = (int16_t)((uint16_t)info->payload[0] | ((uint16_t)info->payload[1] << 8));
    if (reply_enabled && info->type == FRAME_TYPE_JOYSTICK_MODE) {
        len = frame_build(reply, sizeof(reply), FRAME_TYPE_RESPONSE, info->seq, payload, sizeof(payload));
        if (uart_comm_send_copy(reply, len, 9001U) == UART_COMM_ACCEPTED) replies_accepted++;
    }
    __DMB(); frames++;
}
static void on_result(const uart_comm_tx_result_t *result, void *user)
{
    (void)user;
    if (results < 32U) tags[results] = result->tag;
    last_tag = result->tag; last_code = result->code;
    if (result->tag >= 20000U && result->tag < 20100U) {
        if (result->tag != 20000U + producer_results[0]) producer_order_ok = 0U;
        producer_results[0]++;
    }
    if (result->tag >= 30000U && result->tag < 30100U) {
        if (result->tag != 30000U + producer_results[1]) producer_order_ok = 0U;
        producer_results[1]++;
    }
    if (result->code == UART_COMM_TX_COMPLETE) completes++;
    else if (result->code == UART_COMM_TX_CANCELLED_FAULT) cancels++;
    else failures++;
    __DMB(); results++;
}
const uart_comm_handlers_t uart_rtos_test_handlers = {on_frame, on_result, NULL};

static uart_comm_snapshot_t snapshot(void)
{
    uart_comm_snapshot_t out;
    (void)uart_comm_get_snapshot(&out);
    return out;
}
static uint8_t wait_results(uint32_t count, uint32_t timeout)
{
    TickType_t start = xTaskGetTickCount();
    while (results < count && xTaskGetTickCount() - start < pdMS_TO_TICKS(timeout)) vTaskDelay(1U);
    return results == count;
}
static uint8_t wait_ready(uint32_t timeout)
{
    TickType_t start = xTaskGetTickCount();
    uart_comm_snapshot_t s;
    do {
        s = snapshot();
        if (s.rx_ready && s.tx_accepting && s.tx_state == UART_TX_IDLE && s.tx_queue_depth == 0U) return 1U;
        vTaskDelay(1U);
    } while (xTaskGetTickCount() - start < pdMS_TO_TICKS(timeout));
    return 0U;
}
static uint8_t send_joy(uint32_t tag)
{
    uint8_t data[FRAME_MAX_SIZE];
    uint8_t len = frame_build_joystick(data, sizeof(data), 321, -123, (uint16_t)tag);
    return uart_comm_send_copy(data, len, tag) == UART_COMM_ACCEPTED;
}
static void check(unsigned i, uint8_t ok)
{
    uart_comm_test_bool(ids[i], ok);
}
static void producer(void *argument)
{
    uint32_t i;
    (void)argument;
    for (i = 0; i < 100U; i++) {
        while (!send_joy(20000U + i)) vTaskDelay(1U);
        producer_accepted++;
        vTaskDelay(1U);
    }
    producer_done = 1U;
    vTaskSuspend(NULL);
}
__attribute__((noinline)) void uart_rtos_test_done(void) { __asm volatile("nop"); }

static void run(void *argument)
{
    static const char *names[] = {
        "F0_RTOS_HAL_CLOCKS", "F1_PUBLIC_VALIDATION", "F2_FIFO_ACTIVE_PLUS_8",
        "F2_COPY_OWNERSHIP", "F2_FAULT_CANCEL_AND_RECOVER", "F2_LOST_DONE_TIMEOUT",
        "F2_MULTIPLE_PRODUCERS", "F1_PARTIAL_FRAME_DEADLINE", "F3_HANDLER_REPLY",
        "F3_IDLE_NO_POLL_10S", "F3_100HZ_10S", "F3_CONTINUOUS_MAX_FRAME",
        "F3_STACK_MARGIN", "F3_CRITICAL_BUDGET", "F3_IRQ_AND_RX_LATENCY",
        "M2_ACCOUNTING", "M2_RX_TX_INDEPENDENCE", "M2_PHYSICAL_DMA_ERROR", "F3_HANDLER_BUDGET",
        "M2_PUBLIC_FRAGMENTED", "M2_PUBLIC_COMBINED", "M2_PUBLIC_520_WRAP"
    };
    uint8_t data[FRAME_MAX_SIZE], payload[FRAME_MAX_PAYLOAD], len, ok;
    uint32_t base, start_frames, i, start_ms, elapsed, own_accepted, total_cycles;
    TickType_t wake;
    uart_comm_snapshot_t before, after;
    comm_test_profile_t pa, pb;
    (void)argument;
    for (i = 0; i < 22U; i++) ids[i] = uart_comm_test_kaydet(names[i], 11U, 1U);
    start_ms = HAL_GetTick(); wake = xTaskGetTickCount(); vTaskDelay(pdMS_TO_TICKS(20U));
    check(0, wait_ready(200U) && HAL_GetTick() - start_ms >= 19U && xTaskGetTickCount() - wake >= 20U);
    data[0] = 0U;
    check(1, uart_comm_send_copy(NULL, 1U, 0U) == UART_COMM_INVALID &&
             uart_comm_send_copy(data, 0U, 0U) == UART_COMM_INVALID &&
             uart_comm_send_copy(data, 65U, 0U) == UART_COMM_INVALID &&
             uart_comm_send_copy(data, 256U, 0U) == UART_COMM_INVALID &&
             !uart_comm_get_snapshot(NULL) && !uart_comm_request_recovery(0U) &&
             !uart_comm_request_recovery(4U));

    base = results; before = snapshot(); ok = 1U;
    comm_test_pause_owner(1U);
    for (i = 0U; i < 8U; i++) ok &= send_joy(100U + i);
    ok &= !send_joy(999U);
    comm_test_pause_owner(0U);
    ok &= send_joy(108U); ok &= !send_joy(999U);
    ok &= wait_results(base + 9U, 300U);
    for (i = 0U; i < 9U; i++) ok &= tags[base + i] == 100U + i;
    after = snapshot();
    check(2, ok && after.tx_completed - before.tx_completed == 9U && after.tx_queue_high_water == 8U);

    base = results; start_frames = frames;
    len = frame_build_joystick(data, sizeof(data), 777, -42, 500U);
    ok = uart_comm_send_copy(data, len, 500U) == UART_COMM_ACCEPTED;
    memset(data, 0, sizeof(data));
    ok &= wait_results(base + 1U, 100U); vTaskDelay(pdMS_TO_TICKS(3U));
    check(3, ok && frames == start_frames + 1U && seen_x == 777);

    /* Owner kapisi kapandiginda kabul edilmis backlog tek sonuc ile iptal edilir. */
    base = results; before = snapshot(); comm_test_pause_owner(1U);
    for (i = 0U; i < 8U; i++) (void)send_joy(600U + i);
    comm_test_force_fault(); comm_test_pause_owner(0U);
    ok = wait_results(base + 8U, 100U);
    after = snapshot(); ok &= !after.tx_accepting && after.tx_cancelled - before.tx_cancelled == 8U;
    ok &= uart_comm_send_copy(data, 1U, 900U) == UART_COMM_NOT_READY;
    ok &= uart_comm_request_recovery(UART_COMM_RECOVER_TX) && wait_ready(100U);
    check(4, ok);

    base = results; before = snapshot(); tx_test_faults(HAL_OK, 1U, 0U, 0U);
    ok = send_joy(700U) && wait_results(base + 1U, 100U);
    after = snapshot(); tx_test_faults(HAL_OK, 0U, 0U, 0U);
    check(5, ok && last_code == UART_COMM_TX_TIMEOUT && after.tx_timeouts == before.tx_timeouts + 1U && wait_ready(100U));

    base = results; before = snapshot(); own_accepted = 0U;
    (void)xTaskCreateStatic(producer, "TestProducer", 256U, NULL, 24U, producer_stack, &producer_tcb);
    for (i = 0U; i < 100U; i++) {
        while (!send_joy(30000U + i)) vTaskDelay(1U);
        own_accepted++; vTaskDelay(1U);
    }
    while (!producer_done) vTaskDelay(1U);
    ok = wait_results(base + own_accepted + producer_accepted, 1000U);
    after = snapshot();
    check(6, ok && own_accepted == 100U && producer_accepted == 100U && producer_order_ok &&
             producer_results[0] == 100U && producer_results[1] == 100U &&
             after.tx_completed - before.tx_completed == 200U && wait_ready(100U));

    base = results; start_frames = frames;
    len = frame_build_joystick(data, sizeof(data), 1, 2, 900U);
    ok = uart_comm_send_copy(data, 5U, 901U) == UART_COMM_ACCEPTED;
    ok &= wait_results(base + 1U, 100U);
    ok &= uart_comm_send_copy(data + 5U, len - 5U, 902U) == UART_COMM_ACCEPTED;
    ok &= wait_results(base + 2U, 100U); vTaskDelay(pdMS_TO_TICKS(3U));
    check(19, ok && frames == start_frames + 1U);
    base = results; start_frames = frames;
    memcpy(data + len, data, len);
    ok = uart_comm_send_copy(data, 2U * len, 903U) == UART_COMM_ACCEPTED;
    ok &= wait_results(base + 1U, 100U); vTaskDelay(pdMS_TO_TICKS(3U));
    check(20, ok && frames == start_frames + 2U);
    base = results; start_frames = frames; before = snapshot(); ok = 1U;
    for (i = 0U; i < 520U; i++) {
        len = frame_build_joystick(data, sizeof(data), 1, 2, (uint16_t)(65500U + i));
        while (uart_comm_send_copy(data, len, 100000U + i) == UART_COMM_QUEUE_FULL) vTaskDelay(1U);
    }
    ok &= wait_results(base + 520U, 1200U); vTaskDelay(pdMS_TO_TICKS(3U)); after = snapshot();
    check(21, ok && frames == start_frames + 520U && after.rx_overruns == before.rx_overruns &&
              app_proto_state.last_seq == (uint16_t)(65500U + 519U));

    before = snapshot(); base = results;
    len = frame_build_joystick(data, sizeof(data), 1, 2, 1000U);
    (void)len; ok = uart_comm_send_copy(data, 5U, 1000U) == UART_COMM_ACCEPTED;
    ok &= wait_results(base + 1U, 100U); vTaskDelay(pdMS_TO_TICKS(60U)); after = snapshot();
    check(7, ok && after.rx_frame_timeouts == before.rx_frame_timeouts + 1U);

    base = results; start_frames = frames; reply_enabled = 1U; payload[0] = 1U;
    len = frame_build(data, sizeof(data), FRAME_TYPE_JOYSTICK_MODE, 1100U, payload, 1U);
    ok = uart_comm_send_copy(data, len, 1100U) == UART_COMM_ACCEPTED && wait_results(base + 2U, 100U);
    vTaskDelay(pdMS_TO_TICKS(3U)); reply_enabled = 0U;
    check(8, ok && replies_accepted == 1U && frames == start_frames + 2U && last_tag == 9001U);

    (void)wait_ready(100U); vTaskDelay(pdMS_TO_TICKS(120U));
    comm_test_get_profile(&pa); vTaskDelay(pdMS_TO_TICKS(10000U)); comm_test_get_profile(&pb);
    rtos_idle_iterations = pb.iterations - pa.iterations;
    rtos_idle_cycles = pb.task_cycles - pa.task_cycles + pb.irq_cycles - pa.irq_cycles;
    check(9, rtos_idle_iterations == 0U && rtos_idle_cycles == 0U && pb.finite_waits == pa.finite_waits);

    base = results; start_frames = frames; before = snapshot(); ok = 1U;
    comm_test_get_profile(&pa); start_ms = HAL_GetTick(); wake = xTaskGetTickCount();
    for (i = 0U; i < 1000U; i++) {
        ok &= send_joy(40000U + i);
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(10U));
    }
    ok &= wait_results(base + 1000U, 100U); vTaskDelay(pdMS_TO_TICKS(3U));
    elapsed = HAL_GetTick() - start_ms; comm_test_get_profile(&pb); after = snapshot();
    total_cycles = pb.task_cycles - pa.task_cycles + pb.irq_cycles - pa.irq_cycles;
    rtos_100hz_cpu_permille = (uint32_t)((uint64_t)total_cycles * 1000U / ((uint64_t)elapsed * (SystemCoreClock / 1000U)));
    rtos_100hz_frames = frames - start_frames;
    check(10, ok && rtos_100hz_frames == 1000U && rtos_100hz_cpu_permille <= 20U && after.rx_overruns == before.rx_overruns);

    memset(payload, 0x33, sizeof(payload));
    len = frame_build(data, sizeof(data), FRAME_TYPE_RESPONSE, 50000U, payload, sizeof(payload));
    base = results; start_frames = frames; before = snapshot(); own_accepted = 0U;
    comm_test_get_profile(&pa); start_ms = HAL_GetTick();
    while (HAL_GetTick() - start_ms < 1000U) {
        if (uart_comm_send_copy(data, len, 50000U + own_accepted) == UART_COMM_ACCEPTED) own_accepted++;
        else vTaskDelay(1U);
    }
    ok = wait_results(base + own_accepted, 300U); vTaskDelay(pdMS_TO_TICKS(3U));
    elapsed = HAL_GetTick() - start_ms; comm_test_get_profile(&pb); after = snapshot();
    total_cycles = pb.task_cycles - pa.task_cycles + pb.irq_cycles - pa.irq_cycles;
    rtos_stream_cpu_permille = (uint32_t)((uint64_t)total_cycles * 1000U / ((uint64_t)elapsed * (SystemCoreClock / 1000U)));
    rtos_stream_frames = frames - start_frames;
    check(11, ok && own_accepted > 170U && rtos_stream_frames == own_accepted && after.rx_overruns == before.rx_overruns);

    /* FE/NE/PE RX toparlanmasi devam eden saglikli TX'i hata saymamali. */
    base = results; before = snapshot();
    ok = uart_comm_send_copy(data, len, 60000U) == UART_COMM_ACCEPTED;
    huart2.Instance->CR1 |= USART_CR1_SBK;
    ok &= wait_results(base + 1U, 200U) && last_code == UART_COMM_TX_COMPLETE && wait_ready(200U);
    after = snapshot(); check(16, ok && after.rx_restarts > before.rx_restarts);

    base = results; tx_test_faults(HAL_OK, 0U, 0U, 1U);
    ok = send_joy(61000U) && wait_results(base + 1U, 200U);
    tx_test_faults(HAL_OK, 0U, 0U, 0U); after = snapshot();
    uart_comm_test_sonuc(ids[17], ok && last_code == UART_COMM_TX_DMA_ERROR && !after.tx_accepting ?
                         UART_COMM_TEST_PASS : UART_COMM_TEST_FAIL, UART_COMM_TEST_SRC_PHYSICAL,
                         UART_COMM_TX_DMA_ERROR, last_code);
    /* CCM TE deneyinde TC dusuk kalabilir; test fixture durmus periferali sifirlar.
     * Uretim kurtarmasi ayni kanit olmadan gate'i acmaz. */
    if ((huart2.hdmatx->Instance->CR & DMA_SxCR_EN) == 0U && (huart2.Instance->CR3 & USART_CR3_DMAT) == 0U) {
        huart2.Instance->CR1 &= ~USART_CR1_TE; huart2.Instance->CR1 |= USART_CR1_TE;
        vTaskDelay(pdMS_TO_TICKS(2U)); /* Idle frame TC olusumu donusle ayni an degil. */
        (void)uart_comm_request_recovery(UART_COMM_RECOVER_TX | UART_COMM_RECOVER_RX);
        (void)wait_ready(200U);
    }
    after = snapshot();
    check(15, after.tx_queue_depth == 0U && after.tx_state == UART_TX_IDLE &&
              after.tx_accepted == after.tx_completed + after.tx_failed + after.tx_cancelled &&
              after.tx_completed == completes && after.tx_failed == failures && after.tx_cancelled == cancels);
    comm_test_get_profile(&rtos_profile);
    check(12, rtos_profile.stack_free_words >= 128U);
    check(13, rtos_profile.max_critical_cycles < SystemCoreClock / 100000U &&
              app_protocol_test_max_critical_cycles < SystemCoreClock / 100000U);
    check(14, rtos_profile.max_irq_cycles < SystemCoreClock / 1000U && rtos_profile.max_rx_latency_cycles < SystemCoreClock / 200U);
    check(18, rtos_profile.max_frame_handler_cycles < SystemCoreClock / 10000U &&
              rtos_profile.max_result_handler_cycles < SystemCoreClock / 10000U);
    uart_rtos_test_done();
    vTaskSuspend(NULL);
}
void uart_rtos_tests_start(void)
{
    test_task = xTaskCreateStatic(run, "UartTests", 1024U, NULL, 24U, test_stack, &test_tcb);
    configASSERT(test_task != NULL);
}

#ifdef UART_COMM_SERIAL_TEST
/* Harici CH340 fixture: yalniz public API ile gercek, bagimsiz RX/TX.
 * TYPE70 echo, 71 fixture komutu, 72 kart akisi, 73 istatistik, 74 RX sink.
 * Uretim komut protokolu degildir; UART_COMM_TEST olmadan derlenmez. */
static StaticTask_t serial_tcb;
static StackType_t serial_stack[512];
static TaskHandle_t serial_task;
typedef struct { uint16_t seq; uint8_t size, payload[55]; } serial_echo_t;
static QueueHandle_t serial_echo_queue;
static StaticQueue_t serial_echo_queue_cb;
static uint8_t serial_echo_storage[8U * sizeof(serial_echo_t)];
static struct {
    uint32_t frames, echo_accepted, echo_dropped, completed, failed;
    uint32_t stream_accepted, stream_dropped, sink_frames, sink_bad;
    uint32_t sink_expected_count, sink_received;
    uint32_t count;
    uint16_t period, seq;
    uint16_t sink_next_seq;
    uint8_t command, size;
    uint8_t sink_expected_size;
} serial_ctx;

static uint32_t serial_lock(void)
{
    uint32_t saved = __get_PRIMASK();
    __disable_irq();
    return saved;
}
static void serial_unlock(uint32_t saved) { __set_PRIMASK(saved); }
static void serial_drop_echo(void)
{
    uint32_t saved = serial_lock();
    serial_ctx.echo_dropped++;
    serial_unlock(saved);
}
static void serial_put32(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0U; i < 4U; i++) out[i] = (uint8_t)(value >> (8U * i));
}
static void serial_echo(uint16_t seq, const uint8_t *payload, uint8_t size)
{
    uint8_t raw[64], len = frame_build(raw, sizeof(raw), 0x70U, seq, payload, size);
    if (uart_comm_send_copy(raw, len, seq) == UART_COMM_ACCEPTED) serial_ctx.echo_accepted++;
    else serial_drop_echo();
}
static void serial_frame(const frame_info_t *info, void *user)
{
    (void)user;
    serial_ctx.frames++;
    if (info->type == 0x70U) {
        /* 64 baytlik frame_build/CRC de O0'da handler butcesini asabilir.
         * Payload omru dolmadan kopyala; echo'yu fixture taski hazirlasin. */
        serial_echo_t echo = {.seq = info->seq, .size = info->payload_len, .payload = {0}};
        if (info->payload_len != 0U) memcpy(echo.payload, info->payload, info->payload_len);
        if (serial_echo_queue != NULL && xQueueSend(serial_echo_queue, &echo, 0U) == pdPASS) {
            if (serial_task != NULL) xTaskNotifyGive(serial_task);
        } else serial_drop_echo();
    }
    else if (info->type == 0x74U) {
        uint8_t bad = info->seq != serial_ctx.sink_next_seq ||
                      info->payload_len != serial_ctx.sink_expected_size ||
                      serial_ctx.sink_received >= serial_ctx.sink_expected_count;
        serial_ctx.sink_frames++;
        serial_ctx.sink_received++;
        serial_ctx.sink_next_seq++;
        for (unsigned i = 0U; i < info->payload_len; i++) {
            if (info->payload[i] != (uint8_t)(info->seq + i)) {
                bad = 1U;
                break;
            }
        }
        if (bad) serial_ctx.sink_bad++;
    } else if (info->type == 0x71U && info->payload_len == 8U) {
            uint32_t saved = serial_lock();
            serial_ctx.command = info->payload[0];
            serial_ctx.seq = info->seq;
            serial_ctx.period = (uint16_t)info->payload[1] | ((uint16_t)info->payload[2] << 8);
            serial_ctx.size = info->payload[3];
            serial_ctx.count = (uint32_t)info->payload[4] | ((uint32_t)info->payload[5] << 8) |
                               ((uint32_t)info->payload[6] << 16) | ((uint32_t)info->payload[7] << 24);
            if (serial_ctx.command == 1U) {
                serial_ctx.sink_next_seq = 0U; serial_ctx.sink_received = 0U;
                serial_ctx.sink_expected_size = (serial_ctx.size >= 9U && serial_ctx.size <= 64U) ? serial_ctx.size - 9U : 255U;
                serial_ctx.sink_expected_count = serial_ctx.count;
            }
            serial_unlock(saved);
            if (serial_task != NULL) xTaskNotifyGive(serial_task);
    }
}
static void serial_result(const uart_comm_tx_result_t *result, void *user)
{
    (void)user;
    if (result->code == UART_COMM_TX_COMPLETE) serial_ctx.completed++;
    else serial_ctx.failed++;
}
const uart_comm_handlers_t uart_serial_test_handlers = {serial_frame, serial_result, NULL};

static void serial_stats(const comm_test_profile_t *measured)
{
    uart_comm_snapshot_t s;
    comm_test_profile_t p;
    uint32_t values[13], saved;
    uint8_t payload[53], raw[64], len;
    (void)uart_comm_get_snapshot(&s);
    if (measured != NULL) p = *measured;
    else comm_test_get_profile(&p);
    saved = serial_lock();
    values[0] = serial_ctx.frames; values[1] = serial_ctx.echo_accepted;
    values[2] = serial_ctx.echo_dropped; values[3] = serial_ctx.completed;
    values[4] = serial_ctx.failed; values[5] = serial_ctx.stream_accepted;
    values[6] = serial_ctx.stream_dropped; values[9] = serial_ctx.sink_frames;
    values[10] = serial_ctx.sink_bad;
    serial_unlock(saved);
    values[7] = s.rx_overruns; values[8] = s.rx_frame_timeouts;
    values[11] = s.tx_queue_full; values[12] = s.rx_restarts;
    payload[0] = 0U;
    for (unsigned i = 0U; i < 13U; i++) serial_put32(payload + 1U + 4U * i, values[i]);
    len = frame_build(raw, sizeof(raw), 0x73U, 0U, payload, 53U);
    (void)uart_comm_send_copy(raw, len, 0x7300U);
    values[0] = p.task_cycles; values[1] = p.irq_cycles; values[2] = p.iterations;
    values[3] = p.finite_waits; values[4] = p.max_critical_cycles;
    values[5] = p.max_irq_cycles; values[6] = p.max_rx_latency_cycles;
    values[7] = p.stack_free_words; values[8] = p.max_frame_handler_cycles;
    values[9] = p.max_result_handler_cycles; values[10] = p.max_service_cycles;
    values[11] = HAL_GetTick(); payload[0] = 1U;
    for (unsigned i = 0U; i < 12U; i++) serial_put32(payload + 1U + 4U * i, values[i]);
    len = frame_build(raw, sizeof(raw), 0x73U, 1U, payload, 49U);
    (void)uart_comm_send_copy(raw, len, 0x7301U);
}
static void serial_run(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t count, saved;
        uint16_t period, seq;
        uint8_t command, size;
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        saved = serial_lock(); command = serial_ctx.command; count = serial_ctx.count;
        serial_ctx.command = 0U;
        period = serial_ctx.period; size = serial_ctx.size; seq = serial_ctx.seq; serial_unlock(saved);
        if (command == 1U && size >= 9U && size <= 64U && count <= 10000U) {
            uint8_t raw[64], payload[55], len;
            TickType_t wake = xTaskGetTickCount();
            for (uint32_t i = 0U; i < count; i++) {
                for (unsigned j = 0U; j < size - 9U; j++) payload[j] = (uint8_t)(i + j);
                len = frame_build(raw, sizeof(raw), 0x72U, (uint16_t)i, payload, size - 9U);
                uart_comm_send_status_t status;
                do {
                    status = uart_comm_send_copy(raw, len, i);
                    if (status == UART_COMM_QUEUE_FULL) vTaskDelay(1U);
                } while (status == UART_COMM_QUEUE_FULL);
                if (status == UART_COMM_ACCEPTED) serial_ctx.stream_accepted++;
                else serial_ctx.stream_dropped++;
                if (period != 0U) vTaskDelayUntil(&wake, pdMS_TO_TICKS(period));
            }
        } else if (command == 4U) {
            /* Yalniz fixture taski ~200us owner'dan yuksek oncelikle 8+1
             * kabul dener. IRQ'lar aciktir; handler'da toplu is yapilmaz.
             * Onceki oncelik hemen geri gelir, owner kuyrugu normal bosaltir. */
            UBaseType_t priority = uxTaskPriorityGet(NULL);
            vTaskPrioritySet(NULL, 26U);
            for (unsigned i = 0U; i < 9U; i++) serial_echo((uint16_t)(seq + i), NULL, 0U);
            vTaskPrioritySet(NULL, priority);
        } else if (command == 2U) serial_stats(NULL);
        else if (command == 3U && period <= 10000U) {
            comm_test_profile_t a, b;
            vTaskDelay(pdMS_TO_TICKS(20U)); /* Komutun UART servis turu bitsin. */
            comm_test_get_profile(&a);
            vTaskDelay(pdMS_TO_TICKS(period)); comm_test_get_profile(&b);
            b.task_cycles -= a.task_cycles; b.irq_cycles -= a.irq_cycles;
            b.iterations -= a.iterations; b.finite_waits -= a.finite_waits;
            serial_stats(&b);
        }
        serial_echo_t echo;
        while (xQueueReceive(serial_echo_queue, &echo, 0U) == pdPASS)
            serial_echo(echo.seq, echo.payload, echo.size);
    }
}
void uart_serial_tests_start(void)
{
    serial_echo_queue = xQueueCreateStatic(8U, sizeof(serial_echo_t), serial_echo_storage, &serial_echo_queue_cb);
    configASSERT(serial_echo_queue != NULL);
    serial_task = xTaskCreateStatic(serial_run, "SerialTests", 512U, NULL, 24U, serial_stack, &serial_tcb);
    configASSERT(serial_task != NULL);
}
#endif
#endif
