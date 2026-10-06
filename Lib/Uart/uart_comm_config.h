/* Projeye tasirken boyut, task ve sure ayarlari yalniz burada degisir.
 * Derleyici -D ayarlari varsayilanlari ezebilir. RX/TX DMA byte hizali,
 * tamponlar DMA erisebilen SRAM'de olmalidir; F407 CCM kullanilmaz. */
#ifndef UART_COMM_CONFIG_H
#define UART_COMM_CONFIG_H
#ifndef UART_RX_BUF_SIZE
#define UART_RX_BUF_SIZE 256U
#endif
#ifndef UART_RX_SCRATCH_SIZE
#define UART_RX_SCRATCH_SIZE 32U
#endif
#ifndef UART_RX_SERVICE_BUDGET
#define UART_RX_SERVICE_BUDGET 64U
#endif
#ifndef UART_TX_BUF_SIZE
#define UART_TX_BUF_SIZE 64U
#endif
#ifndef COMM_QUEUE_SIZE
#define COMM_QUEUE_SIZE 8U
#endif
#ifndef COMM_STACK_SIZE
#define COMM_STACK_SIZE 512U
#endif
#ifndef COMM_TASK_PRIORITY
#define COMM_TASK_PRIORITY 25U
#endif
#ifndef UART_RX_TIMEOUT_MS
#define UART_RX_TIMEOUT_MS 50U
#endif
#ifndef UART_RX_RESTART_RETRY_MS
#define UART_RX_RESTART_RETRY_MS 5U
#endif
#ifndef UART_RX_RESTART_MAX_TRIES
#define UART_RX_RESTART_MAX_TRIES 5U
#endif
#ifndef UART_RX_SAMPLE_FAIL_MS
#define UART_RX_SAMPLE_FAIL_MS 20U
#endif
#ifndef UART_RX_ABORT_TIMEOUT_MS
#define UART_RX_ABORT_TIMEOUT_MS 20U
#endif
#ifndef UART_RX_RECOVERY_BUDGET_MS
#define UART_RX_RECOVERY_BUDGET_MS 100U
#endif
#ifndef UART_RX_HEALTHY_MS
#define UART_RX_HEALTHY_MS 100U
#endif
#ifndef UART_TX_TIMEOUT_MS
#define UART_TX_TIMEOUT_MS 20U
#endif
#ifndef UART_TX_ABORT_TIMEOUT_MS
#define UART_TX_ABORT_TIMEOUT_MS 20U
#endif

/* uint32 mutlak sayac sarimi sonrasi % ring konumu korunmali: ring 2^n. */
#if UART_RX_BUF_SIZE < 2U || UART_RX_BUF_SIZE > 32768U || (UART_RX_BUF_SIZE & (UART_RX_BUF_SIZE - 1U))
#error "UART_RX_BUF_SIZE must be a power of two, 2..32768"
#endif
#if UART_RX_SCRATCH_SIZE < 1U || UART_RX_SCRATCH_SIZE > UART_RX_BUF_SIZE
#error "UART_RX_SCRATCH_SIZE must be 1..UART_RX_BUF_SIZE"
#endif
#if UART_RX_SERVICE_BUDGET < 1U || UART_RX_SERVICE_BUDGET > 65535U
#error "UART_RX_SERVICE_BUDGET must be 1..65535"
#endif
#if UART_TX_BUF_SIZE < 1U || UART_TX_BUF_SIZE > 65535U
#error "UART_TX_BUF_SIZE must be 1..65535"
#endif
#if COMM_QUEUE_SIZE < 1U || COMM_QUEUE_SIZE > 65535U
#error "COMM_QUEUE_SIZE must be 1..65535"
#endif
#if COMM_STACK_SIZE < 1U || COMM_STACK_SIZE > 65535U
#error "COMM_STACK_SIZE must be 1..65535 words"
#endif
#if COMM_TASK_PRIORITY < 0
#error "COMM_TASK_PRIORITY cannot be negative"
#endif
#if UART_RX_TIMEOUT_MS < 1U || UART_RX_TIMEOUT_MS >= 0x80000000U || \
    UART_RX_RESTART_RETRY_MS < 1U || UART_RX_RESTART_RETRY_MS >= 0x80000000U || \
    UART_RX_SAMPLE_FAIL_MS < 1U || UART_RX_SAMPLE_FAIL_MS >= 0x80000000U || \
    UART_RX_ABORT_TIMEOUT_MS < 1U || UART_RX_ABORT_TIMEOUT_MS >= 0x80000000U || \
    UART_RX_RECOVERY_BUDGET_MS < 1U || UART_RX_RECOVERY_BUDGET_MS >= 0x80000000U || \
    UART_RX_HEALTHY_MS < 1U || UART_RX_HEALTHY_MS >= 0x80000000U || \
    UART_TX_TIMEOUT_MS < 1U || UART_TX_TIMEOUT_MS >= 0x80000000U || \
    UART_TX_ABORT_TIMEOUT_MS < 1U || UART_TX_ABORT_TIMEOUT_MS >= 0x80000000U
#error "UART timeouts must be 1..0x7FFFFFFF ms"
#endif
#if UART_RX_RESTART_MAX_TRIES < 1U || UART_RX_RESTART_MAX_TRIES > 255U
#error "UART_RX_RESTART_MAX_TRIES must be 1..255"
#endif
#endif
