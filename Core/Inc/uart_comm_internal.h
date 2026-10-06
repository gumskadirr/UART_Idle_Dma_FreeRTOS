/* Private UART cekirdegi ve test erisimi; uygulama uart_comm.h kullanir. */
#ifndef UART_COMM_INTERNAL_H
#define UART_COMM_INTERNAL_H
#include "uart_comm.h"
#ifdef UART_COMM_TEST
#define UART_LOCAL
#else
#define UART_LOCAL static __attribute__((unused))
#endif


/* Mevcut 256 bayt Circular DMA ve 115200/8N1 ayari korunur. */
#define UART_RX_BUF_SIZE             256U
#define UART_RX_SCRATCH_SIZE          32U
#define UART_RX_SERVICE_BUDGET        64U
#define UART_RX_FRAME_TIMEOUT_MS     50U
#define UART_RX_RESTART_RETRY_MS       5U
#define UART_RX_RESTART_MAX_TRIES      5U
#define UART_RX_SAMPLE_FAIL_MS       20U
#define UART_RX_ABORT_TIMEOUT_MS     20U
#define UART_RX_RECOVERY_BUDGET_MS   100U
#define UART_RX_HEALTHY_MS           100U



typedef struct {
    /* Kesme yazar. Olay sayilari, bayt sayisi degildir. */
    uint32_t bytes_consumed, late_events;
    volatile uint32_t rx_events;
    volatile uint32_t idle_events;
    volatile uint32_t ht_events;
    volatile uint32_t tc_events;
    volatile uint32_t last_size;
    volatile uint32_t error_events;
    volatile uint32_t abort_complete_events;
    volatile uint32_t last_error;
    /* Owner yazar. Yeniden baslatma gecmis sayaclarini silmez. */
    uint32_t restarts;
    uint32_t restart_fails;
    uint32_t frame_timeouts;
    uint32_t start_fails;
    uint32_t start_rejects;
    uint32_t sample_defers;       /* Tutarsiz sample ertelendi. */
    uint32_t sample_fails;        /* 20 ms gecerli sample alinamadi. */
    uint32_t overruns;
    uint32_t discarded_bytes;     /* Tam tur kaybinda birakilan baytlar. */
    uint32_t copy_rejects;        /* Guvenlik dogrulamasinda reddedilen kopya. */
    uint32_t abort_start_fails;
    uint32_t recovery_fails;      /* Guvenli durus saglanamadi. */
    uint32_t irq_health_events;
    uint8_t faulted;              /* phase == FAULT; durus kaniti degildir. */
} rx_stats_t;

#ifdef UART_COMM_TEST
extern rx_stats_t rx_stats;
#endif
#ifdef UART_COMM_TEST
void comm_test_start_owner(void);
void comm_test_service_once(void);
uint32_t comm_test_next_wait(void);
void comm_test_force_fault(void);
void comm_test_stop_before_scheduler(void);
typedef struct {
    uint32_t task_cycles, irq_cycles, iterations, finite_waits, infinite_waits;
    uint32_t max_service_cycles, max_irq_cycles, max_critical_cycles, max_rx_latency_cycles;
    uint32_t stack_free_words;
    uint32_t max_critical_caller;
    uint32_t max_frame_handler_cycles, max_result_handler_cycles;
} comm_test_profile_t;
void comm_test_get_profile(comm_test_profile_t *out);
void comm_test_frame_enter(void);
void comm_test_frame_exit(void);
void comm_test_irq_enter(void);
void comm_test_irq_exit(void);
void comm_test_pause_owner(uint8_t pause);
#endif

/* Owner API: tek ana dongu/task cagirir. Aktif start HAL_BUSY doner;
 * parser, DMA konumu ve sahiplik degismez. Ilk kurulum init, tekrar discard.
 * HAL_OK: RUNNING; kurulum hatasi HAL_ERROR: FAULT, DMA hala aktif olabilir.
 * Hata donusunden sonra service sinirli toparlanmayi otomatik isletir. */
UART_LOCAL HAL_StatusTypeDef rx_start(UART_HandleTypeDef *uart);
UART_LOCAL void rx_service(void);

/* Handler start oncesi STOPPED/FAULT'ta kurulur. Kisa calisir, HAL/service
 * cagirmaz. Payload yalniz callback suresince gecerlidir; saklanacaksa kopyala. */
UART_LOCAL void rx_set_handler(uart_comm_rx_handler_t handler, void *user);

/* En fazla budget bayt tuketir. 1: hemen islenebilir veri kaldi.
 * Ertelenmis sample 0 doner; sonraki servisi next_wait_ms planlar.
 * Parser yalniz dogrulanmis 32 baytlik scratch'i okur. */
UART_LOCAL uint8_t rx_service_budget(uint16_t budget);

/* 0: hemen is; UINT32_MAX: deadline yok. Frame, sample retry, abort ve
 * restart zamanlarini kapsar. Parser bosken periyodik timeout uyanmasi yok. */
UART_LOCAL uint32_t rx_next_wait_ms(uint32_t now);
UART_LOCAL rx_phase_t rx_get_phase(void);

/* FAULT kurtarma istegi basari sonucu degildir. Owner once durusu dogrular.
 * is_quiescent register/HAL durumunu anlik okur; eski bir bayragi dondurmez. */
UART_LOCAL uint8_t rx_request_recovery(void);
UART_LOCAL uint8_t rx_is_quiescent(void);

/* Owner baglaminda teshis. Gecerli sample yoksa out degismez.
 * uint32 fark aritmetigi kullanilir; >=256 bekleyen bayt konservatif kayip.
 * IRQ'lar birden fazla tur boyunca kapaliysa gercek tur sayisi kurulamaz. */
UART_LOCAL uint8_t rx_get_produced(uint32_t *out);
UART_LOCAL uint32_t rx_get_consumed(void);
UART_LOCAL uint32_t rx_producer_from(uint32_t wrap_base, uint8_t pending_tc, uint32_t ndtr);

/* IRQ/HAL bildirim kapilari. Callback senkron da gelebilir; karar owner'da.
 * UART IRQ cikis kancasi HAL_UART_IRQHandler'dan sonra cagrilir. */
UART_LOCAL void rx_on_error(UART_HandleTypeDef *uart, uint32_t error);
UART_LOCAL void rx_on_abort_complete(UART_HandleTypeDef *uart);


#ifdef UART_COMM_TEST
#define UART_RX_COPY_HOOK_NONE         0U
#define UART_RX_COPY_HOOK_SAMPLE_FAIL  1U
#define UART_RX_COPY_HOOK_OVERWRITE    2U
#define UART_RX_COPY_HOOK_ERROR_GEN    3U
UART_LOCAL void rx_force_restart_fail(uint8_t enable);
UART_LOCAL void rx_test_inject_error(void);
UART_LOCAL void rx_force_start_fail(uint8_t enable);
UART_LOCAL void rx_test_sync_error_on_start(uint8_t enable);
UART_LOCAL void rx_test_force_sample_fail(uint8_t enable);
UART_LOCAL void rx_test_set_copy_hook(uint8_t hook);
UART_LOCAL uint8_t rx_test_get_restart_tries(void);
UART_LOCAL uint8_t rx_test_recovery_active(void);
UART_LOCAL uint32_t rx_test_get_session(void);
UART_LOCAL uint32_t rx_test_get_wrap_base(void);
#endif
#define UART_TX_BUF_SIZE 64U
#define UART_TX_TIMEOUT_MS 20U
#define UART_TX_ABORT_TIMEOUT_MS 20U


typedef enum {
    UART_TX_FAIL_NONE = 0, UART_TX_FAIL_TRANSFER, UART_TX_FAIL_TIMEOUT, UART_TX_FAIL_ABORT
} tx_fail_t;
typedef enum {
    UART_TX_OK = 0, UART_TX_BUSY, UART_TX_INVALID, UART_TX_NOT_READY,
    UART_TX_START_BUSY, UART_TX_START_ERROR
} tx_status_t;
typedef enum {
    UART_TX_RESULT_COMPLETE = 0, UART_TX_RESULT_START_BUSY, UART_TX_RESULT_START_ERROR,
    UART_TX_RESULT_DMA_ERROR, UART_TX_RESULT_TIMEOUT
} tx_result_code_t;
typedef struct {
    tx_result_code_t code;
    uint32_t hal_error;
    bool recovery_fault;
} tx_result_t;
typedef struct {
    volatile uint32_t tx_complete_events, tx_error_events, abort_complete_events;
    uint32_t frames_sent;
    uint32_t bytes_sent;
    uint32_t rejected_busy, rejected_invalid, start_fails;
    uint32_t transfer_errors, transfer_timeouts, abort_start_fails, recovery_fails;
    uint32_t late_completions;
    tx_fail_t last_fail;
    uint32_t last_hal_error;
    uint32_t start_busy, start_errors;
} tx_stats_t;
#ifdef UART_COMM_TEST
extern tx_stats_t tx_stats;
#endif

/* Owner API: tek ana dongu/task. Init aktif TX'i sifirlamaz; FAULT'tan
 * cikis icin EN/DMAT/IRQ kapali, HAL hazir ve UART TC set olmali. */
UART_LOCAL HAL_StatusTypeDef tx_init(UART_HandleTypeDef *uart);

/* 1..64 bayt kopyalanir. OK kabul demektir; teslim sonucu ayri gelir.
 * BUSY modul/tampon/alinmamis sonuc; START_BUSY/ERROR HAL baslangic sonucu.
 * Basarisiz aktarim otomatik tekrar gonderilmez. Kaynak donuste serbesttir. */
UART_LOCAL tx_status_t tx_send_copy(const uint8_t *data, uint16_t len);

/* En fazla bir durum adimi; HAL_Delay veya tamamlanma bekleme dongusu yok.
 * Error + done hata sayilir. Callback fiziksel durus kaniti degildir. */
UART_LOCAL void tx_service(void);
UART_LOCAL tx_state_t tx_get_state(void);
UART_LOCAL uint32_t tx_next_wait_ms(uint32_t now);

/* HAL'e sunulan her deneme bir sonuc verir; INVALID/BUSY/NOT_READY vermez.
 * Sonuc alinmadan sonraki send BUSY. NULL false doner, sonucu tuketmez.
 * recovery_fault orijinal DMA/timeout/baslangic hata nedenini degistirmez. */
UART_LOCAL bool tx_take_result(tx_result_t *out);

/* IRQ bildirimleri: handle eslesir, olay kaydedilir. HAL/state karari yok.
 * Ortak ErrorCallback ErrorCode'u bir kez okur; yalniz DMA hatasi TX'e gelir. */
UART_LOCAL void tx_on_error(UART_HandleTypeDef *uart, uint32_t error);
UART_LOCAL void tx_on_abort_complete(UART_HandleTypeDef *uart);
#ifdef UART_COMM_TEST
/* Yalniz kart kabul deneyi. bad_dma CCM adresiyle gercek DMA TE uretir;
 * digerleri yazilim enjeksiyonudur. Uretim ELF'inde bu kapilar bulunmaz. */
UART_LOCAL void tx_test_faults(HAL_StatusTypeDef start_status, uint8_t drop_done,
                         uint8_t drop_abort, uint8_t bad_dma);
#endif
#endif
