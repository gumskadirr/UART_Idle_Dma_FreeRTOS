#ifndef UART_COMM_H
#define UART_COMM_H
#include <stdbool.h>
#include <stdint.h>
#include "stm32f4xx_hal.h"

typedef enum {
    UART_COMM_RX_DATA, UART_COMM_RX_TIMEOUT, UART_COMM_RX_RESET
} uart_comm_rx_event_t;
#define UART_COMM_RX_PENDING 1U
#define UART_COMM_RX_VALIDATED 2U
/* Task baglami, kritik bolum disinda. DATA isaretcisi yalniz callback'te
 * gecerlidir; TIMEOUT/RESET NULL,0 tasir. RESET pending'i temizler.
 * Donus: eksik mesaj bekliyor / bu cagri dogrulanmis mesaj uretti bitleri. */
typedef uint32_t (*uart_comm_rx_handler_t)(uart_comm_rx_event_t event,
                                          const uint8_t *data, uint16_t len, void *user);

typedef enum {
    UART_RX_PHASE_STOPPED = 0,
    UART_RX_PHASE_STARTING,
    UART_RX_PHASE_RUNNING,
    UART_RX_PHASE_ABORTING,
    UART_RX_PHASE_RETRY_WAIT,
    UART_RX_PHASE_FAULT
} rx_phase_t;
typedef enum { UART_TX_IDLE = 0, UART_TX_SENDING, UART_TX_ABORTING, UART_TX_FAULT } tx_state_t;

typedef enum { UART_COMM_ACCEPTED = 0, UART_COMM_QUEUE_FULL, UART_COMM_INVALID, UART_COMM_NOT_READY } uart_comm_send_status_t;
typedef enum {
    UART_COMM_TX_COMPLETE = 0, UART_COMM_TX_START_BUSY, UART_COMM_TX_START_ERROR,
    UART_COMM_TX_DMA_ERROR, UART_COMM_TX_TIMEOUT, UART_COMM_TX_CANCELLED_FAULT
} uart_comm_tx_code_t;
typedef struct {
    uint32_t tag;
    uart_comm_tx_code_t code;
    uint32_t hal_error;
    bool recovery_fault;
} uart_comm_tx_result_t;
typedef struct {
    uart_comm_rx_handler_t on_rx;
    void (*on_tx_result)(const uart_comm_tx_result_t *, void *);
    void *rx_user;
    void *user;
} uart_comm_handlers_t;
typedef struct {
    rx_phase_t rx_phase;
    tx_state_t tx_state;
    bool initialized, rx_ready, rx_quiescent, tx_accepting, has_tx_failure;
    uart_comm_tx_code_t last_tx_failure;
    uint32_t tx_queue_depth, tx_queue_high_water;
    uint32_t rx_bytes_consumed, rx_overruns, rx_discarded_bytes, rx_timeouts;
    uint32_t rx_start_fails, rx_restarts, rx_recovery_fails, rx_snapshot_defers, rx_late_events;
    uint32_t tx_accepted, tx_completed, tx_failed, tx_cancelled, tx_queue_full;
    uint32_t tx_start_busy, tx_start_errors, tx_dma_errors, tx_timeouts, tx_recovery_fails, tx_late_events;
    uint32_t last_rx_error, last_tx_error;
} uart_comm_snapshot_t;

#define UART_COMM_RECOVER_RX 1U
#define UART_COMM_RECOVER_TX 2U
/* Scheduler oncesi, HAL init sonrasi bir kez. Tek statik UART taski olusturur;
 * RX DMA task baslayinca acilir. Handler/user hedefi modul omru boyunca yasar. */
HAL_StatusTypeDef uart_comm_init(UART_HandleTypeDef *uart, const uart_comm_handlers_t *handlers);
/* Task baglami; 1..64 bayt kopyalanir. ACCEPTED kuyruga kabul demektir.
 * Tag callback'te geri gelir. Sonuc API donmeden gelebilir. Otomatik tekrar yok. */
uart_comm_send_status_t uart_comm_send_copy(const uint8_t *data, uint16_t len, uint32_t tag);
/* true istek kaydidir; fiziksel durus kaniti olmadan yeniden kullanim yok. */
bool uart_comm_request_recovery(uint32_t directions);
/* Kisa korumayla yayinlanmis tutarli kopya; init oncesi initialized=false.
 * Frame/result handler'i UART taskinda kisa calisir, beklemez; send_copy cagirilabilir. */
bool uart_comm_get_snapshot(uart_comm_snapshot_t *out);

/* Yalniz USART IRQ baglantisi: HAL_UART_IRQHandler'dan sonra; uygulama API'si degil. */
void uart_comm_on_rx_event(UART_HandleTypeDef *uart, uint16_t size);
void uart_comm_on_error(UART_HandleTypeDef *uart);
void uart_comm_on_rx_abort_complete(UART_HandleTypeDef *uart);
void uart_comm_on_tx_complete(UART_HandleTypeDef *uart);
void uart_comm_on_tx_abort_complete(UART_HandleTypeDef *uart);
void uart_comm_on_uart_irq_exit(UART_HandleTypeDef *uart);
#endif
