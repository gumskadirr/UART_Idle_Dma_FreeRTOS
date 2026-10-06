/* PC fixture'inin proje callback baglantisi; kutuphane HAL callback tanimlamaz. */
#include "uart_comm.h"
DMA_Stream_TypeDef model_dma_streams[16];
int model_last_cleared_irq = -1;
uint32_t model_pending_clears;
void model_clear_pending_irq(IRQn_Type irq)
{
    model_last_cleared_irq = irq;
    model_pending_clears++;
}
#ifndef UART_PORT_MODEL_ONLY
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *uart, uint16_t size) { uart_comm_on_rx_event(uart, size); }
void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart) { uart_comm_on_error(uart); }
void HAL_UART_AbortReceiveCpltCallback(UART_HandleTypeDef *uart) { uart_comm_on_rx_abort_complete(uart); }
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *uart) { uart_comm_on_tx_complete(uart); }
void HAL_UART_AbortTransmitCpltCallback(UART_HandleTypeDef *uart) { uart_comm_on_tx_abort_complete(uart); }
#endif
