/* Derleme/link entegrasyon ornegi: USART1. Karta yuklenmez.
 * Lib/Uart kaynagini oldugu gibi, protocol/Core uygulamasi olmadan kullanir. */
#include "uart_comm.h"
#include "FreeRTOS.h"
#include "task.h"

static UART_HandleTypeDef uart;
static DMA_HandleTypeDef rx_dma, tx_dma;
static uint8_t last_byte;
static uint32_t received, completed;
static uint32_t on_rx(uart_comm_rx_event_t event, const uint8_t *data, uint16_t len, void *user)
{
    (void)user;
    if (event == UART_COMM_RX_DATA && len != 0U) { last_byte = data[len - 1U]; received += len; }
    return 0U;
}
static void on_tx(const uart_comm_tx_result_t *result, void *user)
{
    (void)user;
    if (result->code == UART_COMM_TX_COMPLETE) completed++;
}
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *u, uint16_t n) { uart_comm_on_rx_event(u, n); }
void HAL_UART_ErrorCallback(UART_HandleTypeDef *u) { uart_comm_on_error(u); }
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *u) { uart_comm_on_tx_complete(u); }
void HAL_UART_AbortReceiveCpltCallback(UART_HandleTypeDef *u) { uart_comm_on_rx_abort_complete(u); }
void HAL_UART_AbortTransmitCpltCallback(UART_HandleTypeDef *u) { uart_comm_on_tx_abort_complete(u); }
void USART1_IRQHandler(void) { HAL_UART_IRQHandler(&uart); uart_comm_on_uart_irq_exit(&uart); }
void DMA2_Stream2_IRQHandler(void) { HAL_DMA_IRQHandler(&rx_dma); }
void DMA2_Stream7_IRQHandler(void) { HAL_DMA_IRQHandler(&tx_dma); }

/* Mevcut kernel statik idle/timer task tamponlarini uygulamadan alir. */
void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack, uint32_t *size)
{
    static StaticTask_t memory;
    static StackType_t words[configMINIMAL_STACK_SIZE];
    *tcb = &memory; *stack = words; *size = configMINIMAL_STACK_SIZE;
}
void vApplicationGetTimerTaskMemory(StaticTask_t **tcb, StackType_t **stack, uint32_t *size)
{
    static StaticTask_t memory;
    static StackType_t words[configTIMER_TASK_STACK_DEPTH];
    *tcb = &memory; *stack = words; *size = configTIMER_TASK_STACK_DEPTH;
}
int main(void)
{
    static const uart_comm_handlers_t handlers = {.on_rx = on_rx, .on_tx_result = on_tx};
    /* Bu fixture link kanitidir; GPIO/saat/NVIC kurulumu yapip calistirilmaz. */
    uart.Instance = USART1;
    uart.Init.WordLength = UART_WORDLENGTH_8B; uart.Init.Parity = UART_PARITY_NONE;
    uart.Init.StopBits = UART_STOPBITS_1;
    uart.hdmarx = &rx_dma; uart.hdmatx = &tx_dma;
    uart.RxState = uart.gState = HAL_UART_STATE_READY;
    rx_dma.Instance = DMA2_Stream2; tx_dma.Instance = DMA2_Stream7;
    rx_dma.Parent = tx_dma.Parent = &uart;
    rx_dma.Init.Channel = tx_dma.Init.Channel = DMA_CHANNEL_4;
    rx_dma.Init.MemInc = tx_dma.Init.MemInc = DMA_MINC_ENABLE;
    rx_dma.Init.PeriphDataAlignment = tx_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    rx_dma.Init.MemDataAlignment = tx_dma.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    rx_dma.Init.Direction = DMA_PERIPH_TO_MEMORY; rx_dma.Init.Mode = DMA_CIRCULAR;
    tx_dma.Init.Direction = DMA_MEMORY_TO_PERIPH; tx_dma.Init.Mode = DMA_NORMAL;
    if (uart_comm_init(&uart, &handlers) != HAL_OK) return 1;
    vTaskStartScheduler();
    return 0;
}
