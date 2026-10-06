#include "uart_comm.h"
#include "uart_comm_internal.h"
#include "uart_comm_port.h"
#include <stddef.h>
#include <string.h>
/* ==================== RTOS / test configuration ==================== */
#if !defined(UART_HAL_MODEL) || defined(UART_RTOS_MODEL)
#define COMM_RTOS 1
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
static void comm_notify(void);
#define COMM_NOTIFY() comm_notify()
#else
#define COMM_NOTIFY() ((void)0)
#endif
/* Test-only DWT olcumleri; uretim derlemesine girmez. */
#if defined(UART_COMM_TEST) && !defined(UART_HAL_MODEL)
static volatile comm_test_profile_t profile;
static uint32_t irq_started, critical_started, critical_caller, rx_notified;
static uint8_t rx_latency_pending;
static uint32_t cycles(void) { return DWT->CYCCNT; }
typedef struct { uint32_t cycles, irq_cycles; } cycle_sample_t;
static cycle_sample_t cycle_sample(void)
{
    uint32_t saved = __get_PRIMASK();
    cycle_sample_t out;
    __disable_irq();
    out.cycles = cycles();
    out.irq_cycles = profile.irq_cycles;
    __set_PRIMASK(saved);
    return out;
}
static uint32_t cycle_elapsed(cycle_sample_t before)
{
    cycle_sample_t after = cycle_sample();
    return after.cycles - before.cycles - (after.irq_cycles - before.irq_cycles);
}
#define COMM_PROFILE 1
#endif
/* ==================== Context / buffers ==================== */
/* Circular RX DMA. RX/TX ve HAL islemlerinin tek sahibi owner tasktir.
 * Kesme yalniz olay kaydeder; DMA bellegi RX callback'ine sabit kopya ile verilir. */

typedef struct {
    uart_comm_rx_handler_t handler;
    void *handler_user;
    volatile rx_phase_t phase;
    uint8_t pending;

    volatile uint32_t wrap_base, session, error_generation;
    uint32_t consumed, last_producer, progress_tick;

    struct {
        volatile uint8_t data, error, health;
        volatile uint32_t fault_tick;
    } events;

    struct {
        uint8_t active;
        uint32_t first_failure, retry_at;
    } sample;

    struct {
        uint8_t active, attempts, final_stop, requested;
        uint32_t started_at, healthy_since, retry_at, abort_at;
    } recovery;
} rx_context_t;

typedef struct {
    uint8_t buffer[UART_TX_BUF_SIZE];
    uint16_t length;
    volatile tx_state_t phase;
    volatile uint8_t active_attempt;
    uint32_t started_at, abort_at;
    tx_result_t result;
    uint8_t result_ready;
    struct { volatile uint8_t done, error, aborted; volatile uint32_t error_code; } events;
} tx_context_t;
#ifdef COMM_RTOS
#if COMM_TASK_PRIORITY >= configMAX_PRIORITIES
#error "COMM_TASK_PRIORITY must be below configMAX_PRIORITIES"
#endif
#if configSUPPORT_STATIC_ALLOCATION != 1 || configUSE_TASK_NOTIFICATIONS != 1 || INCLUDE_xTaskGetSchedulerState != 1
#error "UART needs static allocation, task notifications and scheduler state API"
#endif
typedef struct {
    uint32_t tag, admission_epoch;
    uint16_t len;
    uint8_t bytes[UART_TX_BUF_SIZE];
} tx_item_t;
static StaticTask_t owner_tcb;
static StackType_t owner_stack[COMM_STACK_SIZE];
static StaticQueue_t queue_cb;
static uint8_t queue_storage[COMM_QUEUE_SIZE * sizeof(tx_item_t)];
#endif
static struct {
    UART_HandleTypeDef *uart;
    rx_context_t rx;
    tx_context_t tx;
#ifdef COMM_RTOS
    TaskHandle_t task;
    QueueHandle_t queue;
    uart_comm_handlers_t handlers;
    uart_comm_snapshot_t counters, published;
    uint32_t epoch, recovery_requests;
    bool initialized, accepting, active_valid;
    tx_item_t active;
#ifdef UART_COMM_TEST
    uint8_t test_fault;
#endif
#endif
} comm;
static uint8_t dma_buffer[UART_RX_BUF_SIZE];
UART_LOCAL rx_stats_t rx_stats;

#ifdef UART_COMM_TEST
static struct {
    uint8_t restart_fail, start_fail, sample_fail, copy_hook, sync_error;
} test;
#endif

/* ==================== Shared critical sections / time ==================== */
/* Kritik bolumler yalniz paylasilan olay/register kaydini korur.
 * DMA durmaz. HAL, parser ve uygulama handler'i burada cagrilmaz. */
static uint32_t lock(void)
{
    uint32_t saved = __get_PRIMASK();
    __disable_irq();
#ifdef COMM_PROFILE
    if (saved == 0U) {
        critical_started = cycles();
        critical_caller = (uint32_t)__builtin_return_address(0);
    }
#endif
    return saved;
}

static void unlock(uint32_t saved)
{
#ifdef COMM_PROFILE
    if (saved == 0U) {
        uint32_t elapsed = cycles() - critical_started;
        if (elapsed > profile.max_critical_cycles) {
            profile.max_critical_cycles = elapsed;
            profile.max_critical_caller = critical_caller;
        }
    }
#endif
    __set_PRIMASK(saved);
}

static uint32_t remaining(uint32_t now, uint32_t deadline)
{
    return ((int32_t)(deadline - now) <= 0) ? 0U : deadline - now;
}

/* ==================== RX DMA / byte delivery ==================== */
static void rx_set_phase(rx_phase_t phase)
{
    comm.rx.phase = phase;
    rx_stats.faulted = (uint8_t)(phase == UART_RX_PHASE_FAULT);
}

/* Sahiplik kontrolu: herhangi bir etkin/abort halinde tampon kullanilamaz. */
static uint8_t rx_hardware_active(const UART_HandleTypeDef *uart) { return (uint8_t)uart_port_rx_active(uart); }

/* Saglik kontrolu sahiplikten farklidir: butun kosullar saglanmalidir. */
static uint8_t rx_hardware_healthy(void) { return (uint8_t)uart_port_rx_healthy(comm.uart); }

/* Callback, durus kaniti degildir. Eski RX kaynaklari da kapanmis olmali. */
static uint8_t rx_hardware_stopped(void) { return (uint8_t)uart_port_rx_stopped(comm.uart); }

static uint8_t rx_fault_pending(void)
{
    return (uint8_t)(comm.rx.events.error != 0U || comm.rx.events.health != 0U);
}

static void rx_signal_fault(uint8_t health)
{
    uint32_t now = HAL_GetTick();
    uint32_t saved = lock();
    if (!rx_fault_pending()) comm.rx.events.fault_tick = now;
    if (health) comm.rx.events.health = 1U;
    else comm.rx.events.error = 1U;
    comm.rx.error_generation++;
    unlock(saved);
}

static uint8_t rx_take_fault(uint32_t *when)
{
    uint32_t saved = lock();
    uint8_t pending = rx_fault_pending();
    *when = comm.rx.events.fault_tick;
    comm.rx.events.error = comm.rx.events.health = 0U;
    unlock(saved);
    return pending;
}

/* Yalniz guvenli durustan sonra. HAL start da eski stream bayraklarini siler. */
static void rx_reset_progress(void)
{
    uint32_t saved = lock();
    uart_port_rx_clear_tc(comm.uart);
    comm.rx.wrap_base = comm.rx.consumed = 0U;
    comm.rx.session++;
    comm.rx.events.data = comm.rx.events.error = comm.rx.events.health = 0U;
    unlock(saved);
    comm.rx.sample.active = 0U;
    comm.rx.last_producer = 0U;
    comm.rx.progress_tick = HAL_GetTick();
}

UART_LOCAL uint32_t rx_producer_from(uint32_t wrap_base, uint8_t pending_tc, uint32_t ndtr)
{
    return wrap_base + (pending_tc ? UART_RX_BUF_SIZE : 0U) + UART_RX_BUF_SIZE - ndtr;
}

/* TCIF iki okuma arasinda degisirse veya NDTR yeniden yukleniyorsa tekrar.
 * Bekleyen TC, ISR'nin henuz saymadigi bir turdur; Size degerleri toplanmaz. */
static uint8_t rx_sample_producer(uint32_t *out)
{
    uint8_t attempt;
    if (comm.uart == NULL || out == NULL) return 0U;
#ifdef UART_COMM_TEST
    if (test.sample_fail) return 0U;
#endif
    for (attempt = 0U; attempt < 3U; attempt++) {
        uint32_t saved = lock();
        uint32_t base = comm.rx.wrap_base;
        uart_port_rx_sample_t sample = uart_port_rx_sample(comm.uart);
        uint32_t before = sample.tc_before, ndtr = sample.ndtr, after = sample.tc_after;
        unlock(saved);
        if (before != after || ndtr == 0U || ndtr > UART_RX_BUF_SIZE) continue;
        *out = rx_producer_from(base, (uint8_t)(before != 0U), ndtr);
        return 1U;
    }
    return 0U;
}

/* Tum servis ornekleri ayni erteleme, hata butcesi ve uretim ilerleme saatini kullanir. */
static uint8_t rx_sample_progress(uint32_t *out)
{
    uint32_t now = HAL_GetTick();
    if (comm.rx.sample.active && remaining(now, comm.rx.sample.retry_at) != 0U) return 0U;
    if (rx_sample_producer(out)) {
        comm.rx.sample.active = 0U;
        if (*out != comm.rx.last_producer) {
            comm.rx.last_producer = *out;
            comm.rx.progress_tick = now;
        }
        return 1U;
    }
    if (!comm.rx.sample.active) {
        comm.rx.sample.active = 1U;
        comm.rx.sample.first_failure = now;
    }
    comm.rx.sample.retry_at = now + 1U;
    rx_stats.sample_defers++;
    if ((now - comm.rx.sample.first_failure) >= UART_RX_SAMPLE_FAIL_MS) {
        comm.rx.sample.active = 0U;
        rx_stats.sample_fails++;
        rx_signal_fault(0U);
    }
    return 0U;
}

static void rx_close_recovery(void);
/* Uygulama/protokol callback'i yalniz dogrulanmis kopyayi okur.
 * Donuste yeni IRQ hatasi varsa dogrulanmis mesaj toparlanmayi kapatmaz. */
static void rx_deliver(uart_comm_rx_event_t event, const uint8_t *data, uint16_t len)
{
    uint32_t feedback = 0U, saved;
    if (comm.rx.handler != NULL)
        feedback = comm.rx.handler(event, data, len, comm.rx.handler_user);
    comm.rx.pending = (uint8_t)(event != UART_COMM_RX_RESET &&
                              (feedback & UART_COMM_RX_PENDING) != 0U);
    if (event == UART_COMM_RX_RESET || !(feedback & UART_COMM_RX_VALIDATED)) return;
    saved = lock();
    if (comm.rx.recovery.active && !rx_fault_pending() && rx_hardware_healthy()) rx_close_recovery();
    unlock(saved);
}

#ifdef UART_COMM_TEST
static void rx_copy_test_hook(void)
{
    switch (test.copy_hook) {
    case UART_RX_COPY_HOOK_SAMPLE_FAIL: test.sample_fail = 1U; break;
    case UART_RX_COPY_HOOK_OVERWRITE: comm.rx.wrap_base += UART_RX_BUF_SIZE; break;
    case UART_RX_COPY_HOOK_ERROR_GEN: comm.rx.error_generation++; break;
    default: break;
    }
    test.copy_hook = UART_RX_COPY_HOOK_NONE;
}
#endif

UART_LOCAL uint8_t rx_service_budget(uint16_t budget)
{
#ifdef COMM_PROFILE
    if (rx_latency_pending) {
        uint32_t elapsed = cycles() - rx_notified;
        if (elapsed > profile.max_rx_latency_cycles) profile.max_rx_latency_cycles = elapsed;
        rx_latency_pending = 0U;
    }
#endif
    uint8_t scratch[UART_RX_SCRATCH_SIZE];
    uint32_t produced;
    if (comm.rx.phase != UART_RX_PHASE_RUNNING || rx_fault_pending()) return 0U;

    while (budget != 0U) {
        uint32_t available, count, index, session, generation, after, i;
        if (rx_fault_pending() || !rx_sample_progress(&produced)) return 0U;
        available = produced - comm.rx.consumed;
        if (available == 0U) return 0U;
        if (available >= UART_RX_BUF_SIZE) {
            rx_stats.overruns++;
            rx_stats.discarded_bytes += available;
            rx_deliver(UART_COMM_RX_RESET, NULL, 0U);
            comm.rx.consumed = produced;
            return 1U;
        }

        index = comm.rx.consumed % UART_RX_BUF_SIZE;
        count = available;
        if (count > UART_RX_SCRATCH_SIZE) count = UART_RX_SCRATCH_SIZE;
        if (count > UART_RX_BUF_SIZE - index) count = UART_RX_BUF_SIZE - index;
        if (count > budget) count = budget;
        session = comm.rx.session;
        generation = comm.rx.error_generation;

        __DMB();
        for (i = 0U; i < count; i++) scratch[i] = ((volatile const uint8_t *)dma_buffer)[index + i];
        __DMB();
#ifdef UART_COMM_TEST
        rx_copy_test_hook();
#endif
        if (!rx_sample_progress(&after)) return 0U;
        if (after - comm.rx.consumed >= UART_RX_BUF_SIZE || session != comm.rx.session ||
            generation != comm.rx.error_generation || rx_fault_pending()) {
            rx_stats.copy_rejects++;
            return 1U;
        }

        comm.rx.consumed += count;
        rx_stats.bytes_consumed += count;
        budget -= (uint16_t)count;
        rx_deliver(UART_COMM_RX_DATA, scratch, (uint16_t)count);
    }
    return (uint8_t)(!rx_fault_pending() && rx_sample_progress(&produced) && produced != comm.rx.consumed);
}

/* Timeout veri tuketmez: tur butcesi ikinci kez acilmaz. Yeni veri varsa
 * sonraki tur icin olay birakilir; sample retry suresini de bu yol korur. */
static void rx_service_timeout(void)
{
    uint32_t produced, now = HAL_GetTick();
    if (!comm.rx.pending || comm.rx.events.data || rx_fault_pending() ||
        (now - comm.rx.progress_tick) < UART_RX_TIMEOUT_MS) return;
    if (!rx_sample_progress(&produced)) return;
    if (produced != comm.rx.consumed) {
        comm.rx.events.data = 1U;
        return;
    }
    now = HAL_GetTick();
    if ((now - comm.rx.progress_tick) >= UART_RX_TIMEOUT_MS && !rx_fault_pending()) {
        rx_deliver(UART_COMM_RX_TIMEOUT, NULL, 0U);
        rx_stats.frame_timeouts++;
        comm.rx.progress_tick = now;
    }
}

/* ==================== RX recovery / service ==================== */
static void rx_close_recovery(void)
{
    comm.rx.recovery.active = comm.rx.recovery.attempts = comm.rx.recovery.final_stop = 0U;
}

static void rx_open_recovery(uint32_t now)
{
    if (!comm.rx.recovery.active) {
        comm.rx.recovery.active = 1U;
        comm.rx.recovery.attempts = 0U;
        comm.rx.recovery.started_at = comm.rx.recovery.retry_at = now;
    }
}

static void rx_enter_fault(void)
{
    if (!rx_hardware_stopped()) {
        uint32_t saved = lock();
        /* CR1/CR3 TX ile ortaktir; RX temizligi TX IRQ'nun yazisini ezmemeli. */
        uart_port_rx_mask_sources(comm.uart);
        unlock(saved);
        rx_stats.recovery_fails++;
    }
    rx_close_recovery();
    comm.rx.recovery.requested = 0U;
    rx_set_phase(UART_RX_PHASE_FAULT);
}

static void rx_begin_abort(uint32_t now, uint8_t final_stop)
{
    comm.rx.recovery.abort_at = now;
    comm.rx.recovery.final_stop = final_stop;
    rx_set_phase(UART_RX_PHASE_ABORTING);
    /* HAL'in baslattigi DMA abort'un callback'ini ikinci bir abort ile ezme. */
    if (comm.uart->hdmarx->State == HAL_DMA_STATE_ABORT) return;
    if (HAL_UART_AbortReceive_IT(comm.uart) != HAL_OK) rx_stats.abort_start_fails++;
}

static uint8_t rx_start_hardware(void)
{
    HAL_StatusTypeDef result;
    uint32_t saved;
    uint8_t healthy;
    rx_reset_progress();
    rx_set_phase(UART_RX_PHASE_STARTING);
#ifdef UART_COMM_TEST
    if (test.sync_error) rx_on_error(comm.uart, HAL_UART_ERROR_DMA);
#endif
    result = HAL_UARTEx_ReceiveToIdle_DMA(comm.uart, dma_buffer, sizeof(dma_buffer));
#ifdef UART_COMM_TEST
    if (test.start_fail) {
        test.start_fail = 0U;
        result = HAL_ERROR;
    }
#endif
    saved = lock();
    healthy = (uint8_t)(result == HAL_OK && !rx_fault_pending() && rx_hardware_healthy());
    if (healthy) {
        comm.rx.recovery.healthy_since = HAL_GetTick();
        rx_set_phase(UART_RX_PHASE_RUNNING);
    }
    unlock(saved);
    return healthy;
}

static void rx_try_restart(uint32_t now)
{
    comm.rx.recovery.attempts++;
    comm.rx.recovery.retry_at = now + UART_RX_RESTART_RETRY_MS;
    uart_port_rx_clear_errors(comm.uart);
    comm.uart->ErrorCode = HAL_UART_ERROR_NONE;
    rx_deliver(UART_COMM_RX_RESET, NULL, 0U);
#ifdef UART_COMM_TEST
    if (test.restart_fail) {
        rx_reset_progress();
        rx_stats.restart_fails++;
        return;
    }
#endif
    if (rx_start_hardware()) {
        rx_stats.restarts++;
    } else {
        rx_stats.restart_fails++;
        rx_set_phase(UART_RX_PHASE_RETRY_WAIT);
    }
}

/* Toparlanmayi phase yonetir. Ayrica recover_pending/abort_done bayragi yok. */
static void rx_service_recovery(uint32_t now)
{
    switch (comm.rx.phase) {
    case UART_RX_PHASE_ABORTING:
        if (rx_hardware_stopped()) {
            if (comm.rx.recovery.final_stop) rx_enter_fault();
            else rx_set_phase(UART_RX_PHASE_RETRY_WAIT);
        } else if ((now - comm.rx.recovery.abort_at) >= UART_RX_ABORT_TIMEOUT_MS) {
            rx_enter_fault();
        }
        break;
    case UART_RX_PHASE_RETRY_WAIT:
        if (comm.rx.recovery.attempts >= UART_RX_RESTART_MAX_TRIES ||
            (now - comm.rx.recovery.started_at) >= UART_RX_RECOVERY_BUDGET_MS) {
            if (rx_hardware_stopped()) rx_enter_fault();
            else rx_begin_abort(now, 1U);
        } else if (remaining(now, comm.rx.recovery.retry_at) == 0U) {
            if (rx_hardware_stopped()) rx_try_restart(now);
            else rx_begin_abort(now, 0U);
        }
        break;
    case UART_RX_PHASE_FAULT:
        if (comm.rx.recovery.requested) {
            comm.rx.recovery.requested = 0U;
            rx_open_recovery(now);
            if (rx_hardware_stopped()) rx_set_phase(UART_RX_PHASE_RETRY_WAIT);
            else rx_begin_abort(now, 0U);
        }
        break;
    default: break;
    }
}

UART_LOCAL HAL_StatusTypeDef rx_start(UART_HandleTypeDef *uart)
{
    if (uart == NULL || uart->Instance == NULL || uart->hdmarx == NULL) return HAL_ERROR;
    if (comm.rx.phase == UART_RX_PHASE_STARTING || comm.rx.phase == UART_RX_PHASE_RUNNING ||
        comm.rx.phase == UART_RX_PHASE_ABORTING || rx_hardware_active(comm.uart) || rx_hardware_active(uart)) {
        rx_stats.start_rejects++;
        return HAL_BUSY;
    }
    if (uart->hdmarx->Instance == NULL) return HAL_ERROR;
    comm.uart = uart;
    rx_close_recovery();
    comm.rx.recovery.requested = 0U;
    rx_deliver(UART_COMM_RX_RESET, NULL, 0U);
    if (rx_start_hardware()) return HAL_OK;
    rx_stats.start_fails++;
    rx_set_phase(UART_RX_PHASE_FAULT);
    /* Donus hala gorunur HAL_ERROR/FAULT; owner sonraki turda ayni sinirli
     * durdurma/deneme politikasini isletir. Acilis hatasi firmware'i kilitlemez. */
    rx_open_recovery(HAL_GetTick());
    comm.rx.recovery.requested = 1U;
    return HAL_ERROR;
}

UART_LOCAL void rx_service(void)
{
    uint32_t now = HAL_GetTick(), fault_at;
    uint8_t fault;
    if (comm.uart == NULL) return;
    fault = rx_take_fault(&fault_at);

    /* Saglikli sessizlik sonrasi yeni hata, eski donemin butcesini kullanmaz.
     * Ilk hata zamani saklanir: gec servis, erken gelen hatayi affetmez. */
    if (comm.rx.phase == UART_RX_PHASE_RUNNING && comm.rx.recovery.active &&
        ((fault ? fault_at : now) - comm.rx.recovery.healthy_since) >= UART_RX_HEALTHY_MS) {
        rx_close_recovery();
    }
    if (fault && comm.rx.phase == UART_RX_PHASE_RUNNING) {
        rx_open_recovery(now);
        rx_begin_abort(now, 0U);
    }
    rx_service_recovery(now);
    if (comm.rx.phase != UART_RX_PHASE_RUNNING) return;

    /* IRQ yokken de ertelenmis ornekleme islenir. Timeout ikinci drain yapmaz. */
    if (comm.rx.events.data || comm.rx.sample.active) {
        uint32_t saved = lock();
        comm.rx.events.data = 0U;
        unlock(saved);
        if (rx_service_budget(UART_RX_SERVICE_BUDGET)) comm.rx.events.data = 1U;
    }
    rx_service_timeout();
}

UART_LOCAL uint32_t rx_next_wait_ms(uint32_t now)
{
    if (comm.uart == NULL) return UINT32_MAX;
    if (rx_fault_pending() || comm.rx.recovery.requested) return 0U;
    switch (comm.rx.phase) {
    case UART_RX_PHASE_RUNNING:
        if (comm.rx.sample.active) return remaining(now, comm.rx.sample.retry_at);
        if (comm.rx.events.data) return 0U;
        return comm.rx.pending ? remaining(now, comm.rx.progress_tick + UART_RX_TIMEOUT_MS) : UINT32_MAX;
    case UART_RX_PHASE_ABORTING:
        return remaining(now, comm.rx.recovery.abort_at + UART_RX_ABORT_TIMEOUT_MS);
    case UART_RX_PHASE_RETRY_WAIT: {
        uint32_t retry = remaining(now, comm.rx.recovery.retry_at);
        uint32_t limit = remaining(now, comm.rx.recovery.started_at + UART_RX_RECOVERY_BUDGET_MS);
        return retry < limit ? retry : limit;
    }
    default: return UINT32_MAX;
    }
}

UART_LOCAL void rx_set_handler(uart_comm_rx_handler_t handler, void *user)
{
    if (comm.rx.phase == UART_RX_PHASE_STOPPED || comm.rx.phase == UART_RX_PHASE_FAULT) {
        comm.rx.handler = handler;
        comm.rx.handler_user = user;
    }
}

UART_LOCAL uint8_t rx_request_recovery(void)
{
    if (comm.rx.phase != UART_RX_PHASE_FAULT) return 0U;
    comm.rx.recovery.requested = 1U;
    return 1U;
}

UART_LOCAL rx_phase_t rx_get_phase(void) { return comm.rx.phase; }
UART_LOCAL uint8_t rx_is_quiescent(void) { return rx_hardware_stopped(); }
UART_LOCAL uint8_t rx_get_produced(uint32_t *out) { return rx_sample_producer(out); }
UART_LOCAL uint32_t rx_get_consumed(void) { return comm.rx.consumed; }

/* RX IRQ olay kaydi; servis kararini owner verir. */


UART_LOCAL void rx_on_error(UART_HandleTypeDef *uart, uint32_t error)
{
    if (uart == NULL || uart != comm.uart) return;
    rx_stats.error_events++;
    rx_stats.last_error = error;
    rx_signal_fault(0U);
}

UART_LOCAL void rx_on_abort_complete(UART_HandleTypeDef *uart)
{
    if (uart != NULL && uart == comm.uart) {
        rx_stats.abort_complete_events++;
        COMM_NOTIFY();
    }
}


#ifdef UART_COMM_TEST
UART_LOCAL void rx_force_restart_fail(uint8_t value) { test.restart_fail = value; }
UART_LOCAL void rx_force_start_fail(uint8_t value) { test.start_fail = value; }
UART_LOCAL void rx_test_sync_error_on_start(uint8_t value) { test.sync_error = value; }
UART_LOCAL void rx_test_force_sample_fail(uint8_t value) { test.sample_fail = value; }
UART_LOCAL void rx_test_set_copy_hook(uint8_t value) { test.copy_hook = value; }
UART_LOCAL void rx_test_inject_error(void) { rx_signal_fault(0U); COMM_NOTIFY(); }
UART_LOCAL uint8_t rx_test_get_restart_tries(void) { return comm.rx.recovery.attempts; }
UART_LOCAL uint8_t rx_test_recovery_active(void) { return comm.rx.recovery.active; }
UART_LOCAL uint32_t rx_test_get_session(void) { return comm.rx.session; }
UART_LOCAL uint32_t rx_test_get_wrap_base(void) { return comm.rx.wrap_base; }
#endif

/* ==================== TX DMA ==================== */
/* TX DMA: kesme olay kaydeder, tek owner servis karari verir. */

UART_LOCAL tx_stats_t tx_stats;
#ifdef UART_COMM_TEST
static HAL_StatusTypeDef test_start_status = HAL_OK;
static uint8_t test_drop_done, test_drop_abort, test_bad_dma;
UART_LOCAL void tx_test_faults(HAL_StatusTypeDef status, uint8_t drop_done,
                         uint8_t drop_abort, uint8_t bad_dma)
{
    test_start_status = status;
    test_drop_done = drop_done;
    test_drop_abort = drop_abort;
    test_bad_dma = bad_dma;
}
#endif

typedef struct {
    uint8_t done, error, aborted;
    uint32_t error_code;
} tx_events_t;


static void tx_disable_half_irq(void) { uart_port_tx_disable_half_irq(comm.uart); }

/* Tampon serbestligi ve hat sessizligi ayri ayri dogrulanir. */
static uint8_t tx_hardware_stopped(const UART_HandleTypeDef *handle) { return (uint8_t)uart_port_tx_stopped(handle); }

static void tx_clear_old_sources(void) { uart_port_tx_clear_sources(comm.uart); }

/* Cagiran lock tutar. Yeni olay snapshot ile karar arasinda kaybolmaz. */
static tx_events_t tx_take_events_locked(void)
{
    tx_events_t out = {comm.tx.events.done, comm.tx.events.error, comm.tx.events.aborted, comm.tx.events.error_code};
    comm.tx.events.done = comm.tx.events.error = comm.tx.events.aborted = 0U;
    comm.tx.events.error_code = 0U;
    return out;
}
static tx_events_t tx_take_events(void)
{
    uint32_t saved = lock();
    tx_events_t out = tx_take_events_locked();
    unlock(saved);
    return out;
}
static void tx_merge_events(tx_events_t *pending, tx_events_t extra)
{
    pending->done |= extra.done;
    pending->error |= extra.error;
    pending->aborted |= extra.aborted;
    pending->error_code |= extra.error_code;
}

/* Sonuc tek kutuda tutulur. Tuketilene kadar yeni aktarim kabul edilmez. */
static void tx_finish(tx_state_t next, uint8_t recovery_fault)
{
    comm.tx.phase = next;
    comm.tx.active_attempt = 0U;
    comm.tx.result.recovery_fault = recovery_fault != 0U;
    comm.tx.result_ready = 1U;
    if (recovery_fault) tx_stats.recovery_fails++;
}
static void tx_fail(tx_result_code_t code, uint32_t error, uint32_t now)
{
    comm.tx.result.code = code;
    comm.tx.result.hal_error = error;
    comm.tx.result.recovery_fault = false;
    tx_stats.last_hal_error = error;
    if (code == UART_TX_RESULT_TIMEOUT) {
        tx_stats.transfer_timeouts++;
        tx_stats.last_fail = UART_TX_FAIL_TIMEOUT;
    } else {
        if (code == UART_TX_RESULT_DMA_ERROR) tx_stats.transfer_errors++;
        tx_stats.last_fail = UART_TX_FAIL_TRANSFER;
    }
    comm.tx.abort_at = now;
    comm.tx.active_attempt = 0U;
    comm.tx.phase = UART_TX_ABORTING;
}
static void tx_issue_abort(void)
{
    /* Native HAL'in suren abort callback'ini ikinci cagriyla degistirme. */
    if (comm.uart->hdmatx->State == HAL_DMA_STATE_ABORT) return;
    if (HAL_UART_AbortTransmit_IT(comm.uart) != HAL_OK) tx_stats.abort_start_fails++;
}

UART_LOCAL HAL_StatusTypeDef tx_init(UART_HandleTypeDef *handle)
{
    uint32_t saved;
    if (handle == NULL || handle->Instance == NULL || handle->hdmatx == NULL ||
        handle->hdmatx->Instance == NULL) return HAL_ERROR;
    if (comm.tx.phase == UART_TX_SENDING || comm.tx.phase == UART_TX_ABORTING) return HAL_BUSY;
    if (comm.tx.result_ready) return HAL_BUSY;
    if (!tx_hardware_stopped(comm.uart) || !tx_hardware_stopped(handle)) return HAL_ERROR;
    saved = lock();
    comm.uart = handle;
    comm.tx.length = 0U;
    comm.tx.active_attempt = 0U;
    (void)tx_take_events_locked();
    tx_clear_old_sources();
    comm.tx.phase = UART_TX_IDLE;
    unlock(saved);
    return HAL_OK;
}

UART_LOCAL tx_status_t tx_send_copy(const uint8_t *data, uint16_t len)
{
    HAL_StatusTypeDef status;
    uint32_t saved;
    if (comm.uart == NULL || comm.tx.phase == UART_TX_FAULT) return UART_TX_NOT_READY;
    if (data == NULL || len == 0U || len > UART_TX_BUF_SIZE) {
        tx_stats.rejected_invalid++;
        return UART_TX_INVALID;
    }
    if (comm.tx.phase != UART_TX_IDLE || comm.tx.result_ready) {
        tx_stats.rejected_busy++;
        return UART_TX_BUSY;
    }
    if (!tx_hardware_stopped(comm.uart)) {
        comm.tx.phase = UART_TX_FAULT;
        return UART_TX_NOT_READY;
    }
    memcpy(comm.tx.buffer, data, len);
    saved = lock();
    tx_clear_old_sources();
    (void)tx_take_events_locked();
    comm.tx.length = len;
    comm.tx.started_at = HAL_GetTick();
    comm.tx.active_attempt = 1U;
    comm.tx.phase = UART_TX_SENDING;
    unlock(saved);
    __DMB();
#ifdef UART_COMM_TEST
    /* STM32F407 CCM CPU'ya baglidir, DMA erisemez (RM0090, bus matrix).
     * CPU adresi okumaz; DMA'nin gercek TE kesmesi/HAL yolu sinanir. */
    status = test_start_status != HAL_OK ? test_start_status :
        HAL_UART_Transmit_DMA(comm.uart, test_bad_dma ? (uint8_t *)0x10000000U : comm.tx.buffer, len);
#else
    status = HAL_UART_Transmit_DMA(comm.uart, comm.tx.buffer, len);
#endif
    if (status == HAL_OK) {
        tx_disable_half_irq();
        return UART_TX_OK;
    }

    saved = lock();
    tx_stats.start_fails++;
    if (status == HAL_BUSY) tx_stats.start_busy++;
    else tx_stats.start_errors++;
    tx_fail(status == HAL_BUSY ? UART_TX_RESULT_START_BUSY : UART_TX_RESULT_START_ERROR,
         comm.tx.events.error_code | comm.uart->ErrorCode, HAL_GetTick());
    (void)tx_take_events_locked();
    if (tx_hardware_stopped(comm.uart)) tx_finish(UART_TX_IDLE, 0U);
    unlock(saved);
    if (comm.tx.phase == UART_TX_ABORTING) tx_issue_abort();
    return status == HAL_BUSY ? UART_TX_START_BUSY : UART_TX_START_ERROR;
}

UART_LOCAL void tx_service(void)
{
    tx_events_t pending = tx_take_events();
    uint32_t now, saved;
    uint8_t abort_needed = 0U;
    if (comm.uart == NULL) return;
    if (comm.tx.phase == UART_TX_SENDING) {
        now = HAL_GetTick();
        saved = lock();
        tx_merge_events(&pending, tx_take_events_locked());
        if (pending.error) {
            tx_fail(UART_TX_RESULT_DMA_ERROR, pending.error_code, now);
            abort_needed = 1U;
        } else if (pending.done && tx_hardware_stopped(comm.uart)) {
            comm.tx.result.code = UART_TX_RESULT_COMPLETE;
            comm.tx.result.hal_error = HAL_UART_ERROR_NONE;
            tx_stats.frames_sent++;
            tx_stats.bytes_sent += comm.tx.length;
            tx_finish(UART_TX_IDLE, 0U);
        } else {
            if (pending.done) tx_stats.late_completions++;
            if ((now - comm.tx.started_at) >= UART_TX_TIMEOUT_MS) {
                tx_fail(UART_TX_RESULT_TIMEOUT, HAL_UART_ERROR_NONE, now);
                abort_needed = 1U;
            }
        }
        unlock(saved);
        if (abort_needed) tx_issue_abort();
        return;
    }

    if (pending.done) tx_stats.late_completions++;
    if (comm.tx.phase != UART_TX_ABORTING) return;
    now = HAL_GetTick();
    saved = lock();
    if (tx_hardware_stopped(comm.uart)) {
        tx_finish(UART_TX_IDLE, 0U);
    } else if ((now - comm.tx.abort_at) >= UART_TX_ABORT_TIMEOUT_MS) {
        /* Ortak USART register'larinda RX bitlerini koru. DMA EN zorlanmaz. */
        uart_port_tx_mask_sources(comm.uart);
        tx_finish(UART_TX_FAULT, 1U);
    }
    unlock(saved);
}

UART_LOCAL tx_state_t tx_get_state(void) { return comm.tx.phase; }
UART_LOCAL uint32_t tx_next_wait_ms(uint32_t now)
{
    if (comm.tx.events.done || comm.tx.events.error || comm.tx.events.aborted) return 0U;
    if (comm.tx.phase == UART_TX_SENDING) return remaining(now, comm.tx.started_at + UART_TX_TIMEOUT_MS);
    if (comm.tx.phase == UART_TX_ABORTING) {
        uint32_t wait = remaining(now, comm.tx.abort_at + UART_TX_ABORT_TIMEOUT_MS);
        return wait > 1U ? 1U : wait; /* Yalniz abort'ta kayip callback saglik kontrolu. */
    }
    return UINT32_MAX;
}
UART_LOCAL bool tx_take_result(tx_result_t *out)
{
    if (out == NULL || !comm.tx.result_ready) return false;
    *out = comm.tx.result;
    comm.tx.result_ready = 0U;
    return true;
}

UART_LOCAL void tx_on_error(UART_HandleTypeDef *handle, uint32_t error)
{
    uint32_t saved;
    if (handle == NULL || handle != comm.uart) return;
    saved = lock();
    tx_stats.tx_error_events++;
    if (comm.tx.active_attempt) {
        comm.tx.events.error_code |= error;
        comm.tx.events.error = 1U;
    }
    unlock(saved);
}
UART_LOCAL void tx_on_abort_complete(UART_HandleTypeDef *handle)
{
    uint32_t saved;
    if (handle == NULL || handle != comm.uart) return;
    saved = lock();
    tx_stats.abort_complete_events++;
    comm.tx.events.aborted = 1U;
    unlock(saved);
    COMM_NOTIFY();
}


#ifdef COMM_RTOS
/* ==================== TX queue / result delivery / snapshots ==================== */
static void comm_notify(void)
{
    BaseType_t woken = pdFALSE;
    /* Scheduler suspend iken ISR bildirimi kernel'in pending-ready listesine
     * gider. Son RX IDLE olayini burada dusurmek suresiz uykuda birakabilir. */
    if (comm.task == NULL || xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) return;
    if (__get_IPSR() != 0U) {
        (void)xTaskNotifyFromISR(comm.task, 1U, eSetBits, &woken);
        portYIELD_FROM_ISR(woken);
    } else (void)xTaskNotify(comm.task, 1U, eSetBits);
}

static void comm_close_tx_gate(void)
{
    uint32_t saved = lock();
    if (comm.accepting) {
        comm.accepting = false;
        comm.epoch++;
    }
    unlock(saved);
}

static void comm_deliver_tx_result(uint32_t tag, uart_comm_tx_code_t code, uint32_t error, bool recovery_fault)
{
    uart_comm_tx_result_t out = {tag, code, error, recovery_fault};
    if (code == UART_COMM_TX_COMPLETE) comm.counters.tx_completed++;
    else if (code == UART_COMM_TX_CANCELLED_FAULT) comm.counters.tx_cancelled++;
    else {
        comm.counters.tx_failed++;
        comm.counters.last_tx_failure = code;
        comm.counters.has_tx_failure = true;
    }
    if (comm.handlers.on_tx_result != NULL) {
#ifdef COMM_PROFILE
        cycle_sample_t before = cycle_sample();
#endif
        comm.handlers.on_tx_result(&out, comm.handlers.user);
#ifdef COMM_PROFILE
        uint32_t elapsed = cycle_elapsed(before);
        if (elapsed > profile.max_result_handler_cycles) profile.max_result_handler_cycles = elapsed;
#endif
    }
}

static void comm_publish_snapshot(void)
{
    uart_comm_snapshot_t out;
    uint32_t depth = uxQueueMessagesWaiting(comm.queue), saved;
    saved = lock();
    out = comm.counters;
    unlock(saved);
    /* Owner'in alanlarini IRQ kapatmadan hazirla. Ureticinin degistirdigi
     * kabul/doluluk sayaclari yukaridaki tek kopyada korundu. */
    out.initialized = comm.initialized;
    out.rx_phase = comm.rx.phase;
    out.tx_state = comm.tx.phase;
    out.tx_accepting = comm.accepting;
    out.tx_queue_depth = depth;
    if (depth > out.tx_queue_high_water) comm.counters.tx_queue_high_water = out.tx_queue_high_water = depth;
    out.rx_bytes_consumed = rx_stats.bytes_consumed;
    out.rx_overruns = rx_stats.overruns;
    out.rx_discarded_bytes = rx_stats.discarded_bytes;
    out.rx_timeouts = rx_stats.frame_timeouts;
    out.rx_start_fails = rx_stats.start_fails;
    out.rx_restarts = rx_stats.restarts;
    out.rx_recovery_fails = rx_stats.recovery_fails;
    out.rx_snapshot_defers = rx_stats.sample_defers;
    out.rx_late_events = rx_stats.late_events;
    out.last_tx_error = tx_stats.last_hal_error;
    out.tx_start_busy = tx_stats.start_busy;
    out.tx_start_errors = tx_stats.start_errors;
    out.tx_dma_errors = tx_stats.transfer_errors;
    out.tx_timeouts = tx_stats.transfer_timeouts;
    out.tx_recovery_fails = tx_stats.recovery_fails;
    out.tx_late_events = tx_stats.late_completions;
    saved = lock();
    out.rx_ready = comm.rx.phase == UART_RX_PHASE_RUNNING && rx_hardware_healthy();
    out.rx_quiescent = rx_hardware_stopped() != 0U;
    out.last_rx_error = rx_stats.last_error;
    comm.published = out;
    unlock(saved);
}

/* ==================== Owner service / task ==================== */
static void comm_start_owner(void)
{
    rx_set_handler(comm.handlers.on_rx, comm.handlers.rx_user);
    (void)rx_start(comm.uart); /* HAL_ERROR owner'in sinirli recovery akisina gider. */
    comm_publish_snapshot();
}

/* Aktif ogenin sonucunu callback'e teslim et; callback yeni veri kuyruklayabilir. */
static void comm_deliver_pending_tx_result(void)
{
    tx_result_t out;
    if (tx_take_result(&out) && comm.active_valid) {
        uint32_t tag = comm.active.tag;
        comm.active_valid = false; /* Callback yeniden enqueue yapabilir. */
        comm_deliver_tx_result(tag, (uart_comm_tx_code_t)out.code, out.hal_error, out.recovery_fault);
    }
}

/* Recovery isteklerini bir kez al. TX kuyruğu bosalana kadar istegi koru. */
static void comm_service_recovery_requests(void)
{
    uint32_t saved, recovery;
    saved = lock();
    recovery = comm.recovery_requests;
    comm.recovery_requests = 0U;
    unlock(saved);
    if ((recovery & UART_COMM_RECOVER_RX) && comm.rx.phase == UART_RX_PHASE_FAULT)
        (void)rx_request_recovery();
    if ((recovery & UART_COMM_RECOVER_TX) && comm.tx.phase == UART_TX_FAULT) {
        if (uxQueueMessagesWaiting(comm.queue) != 0U) {
            saved = lock();
            comm.recovery_requests |= UART_COMM_RECOVER_TX;
            unlock(saved);
        } else if (!comm.active_valid && tx_init(comm.uart) == HAL_OK) {
            saved = lock();
            comm.accepting = true;
            unlock(saved);
        }
    }
}

/* Bir turda en fazla bir ogeyi baslat veya FAULT/epoch nedeniyle iptal et. */
static void comm_service_tx_queue(void)
{
    tx_item_t item;
    tx_status_t status;
    /* FAULT bosaltmasi dahil en fazla bir queue ogesi; RX her tur servis alir.
     * SENDING/ABORTING backlog'u dequeue edilmez ve spin sebebi olmaz. */
    if ((!comm.active_valid && comm.tx.phase == UART_TX_IDLE) || comm.tx.phase == UART_TX_FAULT) {
        if (xQueueReceive(comm.queue, &item, 0U) == pdPASS) {
            if (comm.tx.phase == UART_TX_FAULT || item.admission_epoch != comm.epoch) {
                comm_deliver_tx_result(item.tag, UART_COMM_TX_CANCELLED_FAULT, 0U, false);
            } else {
                comm.active = item;
                comm.active_valid = true;
                status = tx_send_copy(item.bytes, item.len);
                if (status == UART_TX_INVALID || status == UART_TX_BUSY || status == UART_TX_NOT_READY) {
                    comm.tx.phase = UART_TX_FAULT;
                    comm_close_tx_gate();
                    comm.active_valid = false;
                    comm_deliver_tx_result(item.tag, UART_COMM_TX_START_ERROR, 0U, true);
                }
            }
        }
    }
}

static void comm_service_once(void)
{
#ifdef COMM_PROFILE
    cycle_sample_t before = cycle_sample();
#endif
    rx_service();
    tx_service();
#ifdef UART_COMM_TEST
    if (comm.test_fault && comm.tx.phase == UART_TX_IDLE) {
        comm.test_fault = 0U;
        comm.tx.phase = UART_TX_FAULT;
    }
#endif
    if (comm.tx.phase == UART_TX_FAULT) comm_close_tx_gate();
    comm_deliver_pending_tx_result();
    comm_service_recovery_requests();
    comm_service_tx_queue();
    comm_publish_snapshot();
#ifdef COMM_PROFILE
    {
        uint32_t elapsed = cycle_elapsed(before);
        profile.task_cycles += elapsed;
        profile.iterations++;
        if (elapsed > profile.max_service_cycles) profile.max_service_cycles = elapsed;
    }
#endif
}

static uint32_t comm_next_wait(void)
{
    uint32_t now = HAL_GetTick(), rx_wait, tx_wait;
    uint32_t saved = lock(), recovery = comm.recovery_requests;
    unlock(saved);
    /* FAULT kuyruğunun son iptalinden sonra ertelenmiş recovery hâlâ iştir;
     * uyandıran notification daha önce tüketilmiş olabilir. */
    if (recovery != 0U) return 0U;
    if (comm.tx.result_ready ||
        ((comm.tx.phase == UART_TX_IDLE || comm.tx.phase == UART_TX_FAULT) &&
         uxQueueMessagesWaiting(comm.queue) != 0U)) return 0U;
    rx_wait = rx_next_wait_ms(now);
    tx_wait = tx_next_wait_ms(now);
    return rx_wait < tx_wait ? rx_wait : tx_wait;
}

static TickType_t comm_wait_ticks(uint32_t ms)
{
    uint64_t ticks;
    if (ms == UINT32_MAX) return portMAX_DELAY;
    ticks = ((uint64_t)ms * configTICK_RATE_HZ + 999U) / 1000U;
    return ticks >= portMAX_DELAY ? portMAX_DELAY - 1U : (TickType_t)ticks;
}

static void UartCommTask(void *argument)
{
    uint32_t events, wait;
    (void)argument;
    comm_start_owner();
    for (;;) {
        comm_service_once();
        wait = comm_next_wait(); /* Handler sonrasi guncel zaman. */
        if (wait == 0U) continue;
#ifdef COMM_PROFILE
        if (wait == UINT32_MAX) profile.infinite_waits++;
        else profile.finite_waits++;
#endif
        /* Giriste notification temizlenmez: uyku oncesi gelen olay korunur.
         * Bildirim yalniz uyandirir; ham olaylar ikinci kez enjekte edilmez. */
        (void)xTaskNotifyWait(0U, UINT32_MAX, &events, comm_wait_ticks(wait));
    }
}

/* ==================== Public API ==================== */
HAL_StatusTypeDef uart_comm_init(UART_HandleTypeDef *uart, const uart_comm_handlers_t *handlers)
{
    if (comm.initialized) return HAL_BUSY;
    if (__get_IPSR() || handlers == NULL || !uart_port_validate(uart)) return HAL_ERROR;
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) return HAL_BUSY;
    if (rx_hardware_active(uart) || tx_init(uart) != HAL_OK) return HAL_ERROR;
    comm.uart = uart;
    comm.handlers = *handlers;
    comm.queue = xQueueCreateStatic(COMM_QUEUE_SIZE, sizeof(tx_item_t), queue_storage, &queue_cb);
    if (comm.queue == NULL) return HAL_ERROR;
    comm.task = xTaskCreateStatic(UartCommTask, "UartCommTask", COMM_STACK_SIZE, NULL,
                                COMM_TASK_PRIORITY, owner_stack, &owner_tcb);
    if (comm.task == NULL) return HAL_ERROR;
#ifdef COMM_PROFILE
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    memset((void *)&profile, 0, sizeof(profile));
    rx_latency_pending = 0U;
#endif
    comm.initialized = comm.accepting = true;
    comm_publish_snapshot();
    return HAL_OK;
}

uart_comm_send_status_t uart_comm_send_copy(const uint8_t *data, uint16_t len, uint32_t tag)
{
    tx_item_t item;
    uint32_t saved, epoch;
    bool accepting;
    if (__get_IPSR() || data == NULL || len == 0U || len > UART_TX_BUF_SIZE) return UART_COMM_INVALID;
    if (!comm.initialized || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return UART_COMM_NOT_READY;
    saved = lock();
    accepting = comm.accepting;
    epoch = comm.epoch;
    unlock(saved);
    if (!accepting) return UART_COMM_NOT_READY;
    memset(&item, 0, sizeof(item));
    item.tag = tag;
    item.admission_epoch = epoch;
    item.len = len;
    memcpy(item.bytes, data, len);
    if (xQueueSendToBack(comm.queue, &item, 0U) != pdPASS) {
        saved = lock();
        comm.counters.tx_queue_full++;
        unlock(saved);
        comm_notify(); /* Yeni sayac snapshot'i da owner'da yayimlanir. */
        return UART_COMM_QUEUE_FULL;
    }
    saved = lock();
    comm.counters.tx_accepted++;
    unlock(saved);
    comm_notify();
    return UART_COMM_ACCEPTED;
}

bool uart_comm_request_recovery(uint32_t directions)
{
    uint32_t saved;
    if (__get_IPSR() || !comm.initialized || directions == 0U ||
        (directions & ~(UART_COMM_RECOVER_RX | UART_COMM_RECOVER_TX)) != 0U) return false;
    saved = lock();
    comm.recovery_requests |= directions;
    unlock(saved);
    comm_notify();
    return true;
}

bool uart_comm_get_snapshot(uart_comm_snapshot_t *out)
{
    uint32_t saved;
    if (out == NULL) return false;
    saved = lock();
    *out = comm.published;
    unlock(saved);
    return true;
}

/* ==================== Owner test hooks ==================== */
#ifdef UART_COMM_TEST
void comm_test_start_owner(void) { comm_start_owner(); }
void comm_test_service_once(void) { comm_service_once(); }
uint32_t comm_test_next_wait(void) { return comm_next_wait(); }
void comm_test_force_fault(void) { comm.test_fault = 1U; comm_notify(); }
void comm_test_stop_before_scheduler(void)
{
    uint32_t start = HAL_GetTick();
    configASSERT(xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED);
    rx_force_restart_fail(1U);
    rx_test_inject_error();
    while ((HAL_GetTick() - start) < 100U) { rx_service(); tx_service(); }
    rx_force_restart_fail(0U);
}
#ifdef COMM_PROFILE
void comm_test_get_profile(comm_test_profile_t *out)
{
    uint32_t free_words = uxTaskGetStackHighWaterMark(comm.task), saved = lock();
    profile.stack_free_words = free_words;
    *out = profile;
    unlock(saved);
}
static cycle_sample_t frame_before;
void comm_test_frame_enter(void) { frame_before = cycle_sample(); }
void comm_test_frame_exit(void)
{
    uint32_t elapsed = cycle_elapsed(frame_before);
    if (elapsed > profile.max_frame_handler_cycles) profile.max_frame_handler_cycles = elapsed;
}
void comm_test_irq_enter(void) { irq_started = cycles(); }
void comm_test_irq_exit(void)
{
    uint32_t elapsed = cycles() - irq_started;
    profile.irq_cycles += elapsed;
    if (elapsed > profile.max_irq_cycles) profile.max_irq_cycles = elapsed;
}
void comm_test_pause_owner(uint8_t pause)
{
    if (pause) vTaskSuspend(comm.task);
    else vTaskResume(comm.task);
}
#endif
#endif
#endif /* COMM_RTOS */

/* ==================== HAL callbacks / IRQ ==================== */

void uart_comm_on_rx_event(UART_HandleTypeDef *uart, uint16_t size)
{
    HAL_UART_RxEventTypeTypeDef type;
    uint32_t saved;
    if (uart == NULL || uart != comm.uart) return;
    type = HAL_UARTEx_GetRxEventType(uart);
    saved = lock();
    rx_stats.rx_events++;
    rx_stats.last_size = size;
    comm.rx.events.data = 1U;
#ifdef COMM_PROFILE
    if (!rx_latency_pending) {
        rx_notified = cycles();
        rx_latency_pending = 1U;
    }
#endif
    switch (type) {
    case HAL_UART_RXEVENT_TC:
        rx_stats.tc_events++;
        if (comm.rx.phase == UART_RX_PHASE_RUNNING || comm.rx.phase == UART_RX_PHASE_STARTING)
            comm.rx.wrap_base += UART_RX_BUF_SIZE;
        else rx_stats.late_events++;
        break;
    case HAL_UART_RXEVENT_HT: rx_stats.ht_events++; break;
    case HAL_UART_RXEVENT_IDLE: rx_stats.idle_events++; break;
    default: break;
    }
    unlock(saved);
    COMM_NOTIFY();
}

void uart_comm_on_uart_irq_exit(UART_HandleTypeDef *uart)
{
    if (uart == NULL || uart != comm.uart) return;
    if (comm.rx.phase == UART_RX_PHASE_RUNNING && !rx_hardware_healthy() && !comm.rx.events.health) {
        rx_stats.irq_health_events++;
        rx_signal_fault(1U);
        COMM_NOTIFY();
    }
}

void uart_comm_on_error(UART_HandleTypeDef *uart)
{
    uint32_t error;
    if (uart == NULL || uart != comm.uart) return;
    error = uart->ErrorCode;
    rx_on_error(uart, error);
    if ((error & HAL_UART_ERROR_DMA) != 0U) tx_on_error(uart, error);
    /* Thread baglaminda notify owner'i hemen calistirabilir: iki yonun ham
     * kaydi tamamlanmadan bildirim verme. */
    COMM_NOTIFY();
}

void uart_comm_on_rx_abort_complete(UART_HandleTypeDef *uart) { rx_on_abort_complete(uart); }

void uart_comm_on_tx_complete(UART_HandleTypeDef *handle)
{
    uint32_t saved;
    if (handle == NULL || handle != comm.uart) return;
#ifdef UART_COMM_TEST
    if (test_drop_done) return;
#endif
    saved = lock();
    tx_stats.tx_complete_events++;
    comm.tx.events.done = 1U;
    unlock(saved);
    COMM_NOTIFY();
}

void uart_comm_on_tx_abort_complete(UART_HandleTypeDef *handle)
{
    if (handle == NULL || handle != comm.uart) return;
#ifdef UART_COMM_TEST
    if (test_drop_abort) return;
#endif
    tx_on_abort_complete(handle);
}
