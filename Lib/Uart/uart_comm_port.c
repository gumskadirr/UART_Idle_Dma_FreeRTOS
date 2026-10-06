#include "uart_comm_port.h"
#include <stddef.h>

bool uart_port_dma_irq(DMA_Stream_TypeDef *stream, IRQn_Type *out)
{
    static const struct { DMA_Stream_TypeDef *stream; IRQn_Type irq; } streams[] = {
        {DMA1_Stream0, DMA1_Stream0_IRQn}, {DMA1_Stream1, DMA1_Stream1_IRQn},
        {DMA1_Stream2, DMA1_Stream2_IRQn}, {DMA1_Stream3, DMA1_Stream3_IRQn},
        {DMA1_Stream4, DMA1_Stream4_IRQn}, {DMA1_Stream5, DMA1_Stream5_IRQn},
        {DMA1_Stream6, DMA1_Stream6_IRQn}, {DMA1_Stream7, DMA1_Stream7_IRQn},
        {DMA2_Stream0, DMA2_Stream0_IRQn}, {DMA2_Stream1, DMA2_Stream1_IRQn},
        {DMA2_Stream2, DMA2_Stream2_IRQn}, {DMA2_Stream3, DMA2_Stream3_IRQn},
        {DMA2_Stream4, DMA2_Stream4_IRQn}, {DMA2_Stream5, DMA2_Stream5_IRQn},
        {DMA2_Stream6, DMA2_Stream6_IRQn}, {DMA2_Stream7, DMA2_Stream7_IRQn}
    };
    unsigned i;
    if (stream == NULL || out == NULL) return false;
    for (i = 0U; i < sizeof(streams) / sizeof(streams[0]); i++) {
        if (stream == streams[i].stream) { *out = streams[i].irq; return true; }
    }
    return false;
}

bool uart_port_validate(const UART_HandleTypeDef *uart)
{
    IRQn_Type irq;
    if (uart == NULL || uart->Instance == NULL || uart->hdmarx == NULL || uart->hdmatx == NULL ||
        uart->hdmarx->Instance == uart->hdmatx->Instance) return false;
    if (!uart_port_dma_irq(uart->hdmarx->Instance, &irq) ||
        !uart_port_dma_irq(uart->hdmatx->Instance, &irq)) return false;
#ifndef UART_HAL_MODEL
    if (!IS_UART_INSTANCE(uart->Instance)) return false;
#endif
    /* Byte tamponu sozlesmesi: 9 bit/veri veya genis DMA elemani SRAM'i
     * asabilir. Port bu paket icin desteklenen 8N1/byte ayarini dogrular. */
    if (uart->Init.WordLength != UART_WORDLENGTH_8B || uart->Init.Parity != UART_PARITY_NONE ||
        uart->Init.StopBits != UART_STOPBITS_1 ||
        uart->hdmarx->Init.MemInc != DMA_MINC_ENABLE || uart->hdmatx->Init.MemInc != DMA_MINC_ENABLE ||
        uart->hdmarx->Init.PeriphDataAlignment != DMA_PDATAALIGN_BYTE ||
        uart->hdmarx->Init.MemDataAlignment != DMA_MDATAALIGN_BYTE ||
        uart->hdmatx->Init.PeriphDataAlignment != DMA_PDATAALIGN_BYTE ||
        uart->hdmatx->Init.MemDataAlignment != DMA_MDATAALIGN_BYTE) return false;
    return uart->hdmarx->Parent == uart && uart->hdmatx->Parent == uart &&
        uart->hdmarx->Init.Direction == DMA_PERIPH_TO_MEMORY && uart->hdmarx->Init.Mode == DMA_CIRCULAR &&
        uart->hdmatx->Init.Direction == DMA_MEMORY_TO_PERIPH && uart->hdmatx->Init.Mode == DMA_NORMAL;
}

bool uart_port_rx_active(const UART_HandleTypeDef *uart)
{
    if (uart == NULL) return false;
    if (uart->RxState == HAL_UART_STATE_BUSY_RX || READ_BIT(uart->Instance->CR3, USART_CR3_DMAR)) return true;
    if (uart->hdmarx == NULL || uart->hdmarx->Instance == NULL) return false;
    return READ_BIT(uart->hdmarx->Instance->CR, DMA_SxCR_EN) != 0U ||
           uart->hdmarx->State == HAL_DMA_STATE_BUSY || uart->hdmarx->State == HAL_DMA_STATE_ABORT;
}
bool uart_port_rx_healthy(const UART_HandleTypeDef *uart)
{
    return uart->RxState == HAL_UART_STATE_BUSY_RX && uart->ReceptionType == HAL_UART_RECEPTION_TOIDLE &&
        uart->hdmarx->State == HAL_DMA_STATE_BUSY && READ_BIT(uart->Instance->CR3, USART_CR3_DMAR) != 0U &&
        READ_BIT(uart->hdmarx->Instance->CR, DMA_SxCR_EN) != 0U;
}
bool uart_port_rx_stopped(const UART_HandleTypeDef *uart)
{
    if (uart == NULL) return false;
    return uart->RxState == HAL_UART_STATE_READY && uart->hdmarx->State == HAL_DMA_STATE_READY &&
        READ_BIT(uart->Instance->CR3, USART_CR3_DMAR | USART_CR3_EIE) == 0U &&
        READ_BIT(uart->Instance->CR1, USART_CR1_IDLEIE | USART_CR1_RXNEIE | USART_CR1_PEIE) == 0U &&
        READ_BIT(uart->hdmarx->Instance->CR, DMA_SxCR_EN) == 0U;
}
bool uart_port_tx_stopped(const UART_HandleTypeDef *uart)
{
    if (uart == NULL) return true;
    return uart->gState == HAL_UART_STATE_READY && uart->hdmatx->State == HAL_DMA_STATE_READY &&
        READ_BIT(uart->hdmatx->Instance->CR, DMA_SxCR_EN) == 0U &&
        READ_BIT(uart->Instance->CR3, USART_CR3_DMAT) == 0U &&
        READ_BIT(uart->Instance->CR1, USART_CR1_TXEIE | USART_CR1_TCIE) == 0U &&
        READ_BIT(uart->Instance->SR, USART_SR_TC) != 0U;
}

uart_port_rx_sample_t uart_port_rx_sample(const UART_HandleTypeDef *uart)
{
    DMA_HandleTypeDef *dma = uart->hdmarx;
    uart_port_rx_sample_t out;
    out.tc_before = __HAL_DMA_GET_FLAG(dma, __HAL_DMA_GET_TC_FLAG_INDEX(dma));
    out.ndtr = __HAL_DMA_GET_COUNTER(dma);
    out.tc_after = __HAL_DMA_GET_FLAG(dma, __HAL_DMA_GET_TC_FLAG_INDEX(dma));
    return out;
}
void uart_port_rx_clear_tc(UART_HandleTypeDef *uart)
{
    __HAL_DMA_CLEAR_FLAG(uart->hdmarx, __HAL_DMA_GET_TC_FLAG_INDEX(uart->hdmarx));
}
void uart_port_rx_mask_sources(UART_HandleTypeDef *uart)
{
    /* RX ve TX CR1/CR3'u paylasir. Cagiran kilidi TX IRQ yazisini korur. */
    CLEAR_BIT(uart->Instance->CR3, USART_CR3_DMAR | USART_CR3_EIE);
    CLEAR_BIT(uart->Instance->CR1, USART_CR1_IDLEIE | USART_CR1_RXNEIE | USART_CR1_PEIE);
}
void uart_port_rx_clear_errors(UART_HandleTypeDef *uart) { __HAL_UART_CLEAR_OREFLAG(uart); }
void uart_port_tx_disable_half_irq(UART_HandleTypeDef *uart)
{
    /* HTIE bit 3: aktif DMA EN bitini eski degerle geri yazma. */
#ifdef UART_HAL_MODEL
    model_bitband_clear(&uart->hdmatx->Instance->CR, DMA_IT_HT);
#else
    uintptr_t alias = PERIPH_BB_BASE +
        ((uintptr_t)&uart->hdmatx->Instance->CR - PERIPH_BASE) * 32U + 3U * 4U;
    *(volatile uint32_t *)alias = 0U;
#endif
}
void uart_port_tx_clear_sources(UART_HandleTypeDef *uart)
{
    DMA_HandleTypeDef *dma = uart->hdmatx;
    IRQn_Type irq;
    __HAL_DMA_CLEAR_FLAG(dma, __HAL_DMA_GET_TC_FLAG_INDEX(dma) | __HAL_DMA_GET_HT_FLAG_INDEX(dma) |
        __HAL_DMA_GET_TE_FLAG_INDEX(dma) | __HAL_DMA_GET_DME_FLAG_INDEX(dma) | __HAL_DMA_GET_FE_FLAG_INDEX(dma));
    /* Yalniz TX DMA pending; ortak UART IRQ pending kaydi korunur. */
    if (uart_port_dma_irq(dma->Instance, &irq)) HAL_NVIC_ClearPendingIRQ(irq);
}
void uart_port_tx_mask_sources(UART_HandleTypeDef *uart)
{
    CLEAR_BIT(uart->Instance->CR3, USART_CR3_DMAT);
    CLEAR_BIT(uart->Instance->CR1, USART_CR1_TXEIE | USART_CR1_TCIE);
}
