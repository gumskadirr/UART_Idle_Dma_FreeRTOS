#ifndef UART_RX_HAL_MODEL_H
#define UART_RX_HAL_MODEL_H
#include <stdint.h>
#define UART_HAL_MODEL 1
#ifdef UART_RTOS_MODEL
extern uint32_t model_ipsr;
#define __get_IPSR() model_ipsr
#endif

/* Yalniz PC testleri: UART/DMA register'lari ve HAL yan etkilerinin modeli.
 * Donanim zamanlamasi, bus gecikmesi ve IRQ oncelikleri taklit edilmez. */

typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef enum { HAL_UART_STATE_READY, HAL_UART_STATE_BUSY_RX, HAL_UART_STATE_BUSY_TX } HAL_UART_StateTypeDef;
typedef enum { HAL_DMA_STATE_READY, HAL_DMA_STATE_BUSY, HAL_DMA_STATE_ABORT } HAL_DMA_StateTypeDef;
typedef enum { HAL_UART_RXEVENT_TC, HAL_UART_RXEVENT_HT, HAL_UART_RXEVENT_IDLE } HAL_UART_RxEventTypeTypeDef;
typedef struct { volatile uint32_t CR, NDTR, flags; } DMA_Stream_TypeDef;
typedef struct {
    DMA_Stream_TypeDef *Instance; HAL_DMA_StateTypeDef State;
    void *Parent;
    struct { uint32_t Direction, Mode; } Init;
} DMA_HandleTypeDef;
#define DMA_PERIPH_TO_MEMORY 0U
#define DMA_MEMORY_TO_PERIPH 1U
#define DMA_CIRCULAR 1U
#define DMA_NORMAL 0U
typedef struct { volatile uint32_t CR1, CR3, SR, DR; } USART_TypeDef;
typedef struct {
    USART_TypeDef *Instance;
    DMA_HandleTypeDef *hdmarx;
    HAL_UART_StateTypeDef RxState;
    HAL_UART_RxEventTypeTypeDef RxEventType;
    uint32_t ErrorCode, ReceptionType;
    DMA_HandleTypeDef *hdmatx;
    HAL_UART_StateTypeDef gState;
} UART_HandleTypeDef;

#define USART_CR3_DMAR (1U << 6)
#define USART_CR3_EIE  1U
#define USART_CR3_DMAT (1U << 7)
#define USART_CR1_TXEIE (1U << 7)
#define USART_SR_TC (1U << 6)
#define USART_CR1_IDLEIE (1U << 4)
#define USART_CR1_RXNEIE (1U << 5)
#define USART_CR1_PEIE (1U << 8)
#define USART_CR1_TCIE (1U << 6)
#define DMA_SxCR_EN 1U
#define DMA_IT_HT (1U << 3)
typedef enum {
    DMA1_Stream0_IRQn=11, DMA1_Stream1_IRQn=12, DMA1_Stream2_IRQn=13, DMA1_Stream3_IRQn=14,
    DMA1_Stream4_IRQn=15, DMA1_Stream5_IRQn=16, DMA1_Stream6_IRQn=17, DMA1_Stream7_IRQn=47,
    DMA2_Stream0_IRQn=56, DMA2_Stream1_IRQn=57, DMA2_Stream2_IRQn=58, DMA2_Stream3_IRQn=59,
    DMA2_Stream4_IRQn=60, DMA2_Stream5_IRQn=68, DMA2_Stream6_IRQn=69, DMA2_Stream7_IRQn=70
} IRQn_Type;
extern DMA_Stream_TypeDef model_dma_streams[16];
void model_clear_pending_irq(IRQn_Type irq);
#define HAL_NVIC_ClearPendingIRQ(irq) model_clear_pending_irq(irq)
#define __HAL_DMA_DISABLE_IT(dma, bit) CLEAR_BIT((dma)->Instance->CR, (bit))
#define UART_IT_IDLE USART_CR1_IDLEIE
#define HAL_UART_ERROR_NONE 0U
#define HAL_UART_ERROR_FE 4U
#define HAL_UART_ERROR_DMA 16U
#define SET_BIT(reg, bit) ((reg) |= (bit))
#define HAL_UART_RECEPTION_STANDARD 0U
#define HAL_UART_RECEPTION_TOIDLE 1U
#define READ_BIT(reg, bit) ((reg) & (bit))
void model_clear_bit(volatile uint32_t *reg, uint32_t bit);
#define CLEAR_BIT(reg, bit) model_clear_bit(&(reg), (bit))
#define __HAL_DMA_GET_FLAG(dma, flag) ((dma)->Instance->flags & (flag))
#define __HAL_DMA_GET_TC_FLAG_INDEX(dma) 1U
#define __HAL_DMA_GET_HT_FLAG_INDEX(dma) 2U
#define __HAL_DMA_GET_TE_FLAG_INDEX(dma) 4U
#define __HAL_DMA_GET_DME_FLAG_INDEX(dma) 8U
#define __HAL_DMA_GET_FE_FLAG_INDEX(dma) 16U
#define __HAL_DMA_CLEAR_FLAG(dma, flag) ((dma)->Instance->flags &= ~(flag))
#define __HAL_DMA_GET_COUNTER(dma) ((dma)->Instance->NDTR)
#define __HAL_UART_CLEAR_OREFLAG(uart) ((uart)->Instance->SR &= ~0x0FU)
#define __HAL_UART_DISABLE_IT(uart, bit) CLEAR_BIT((uart)->Instance->CR1, bit)
extern uint32_t model_primask;
#define __get_PRIMASK() model_primask
#define __disable_irq() (model_primask = 1U)
void model_set_primask(uint32_t value);
void model_bitband_clear(volatile uint32_t *reg, uint32_t bit);
#define __set_PRIMASK(value) model_set_primask(value)
#define __DMB() ((void)0)

uint32_t HAL_GetTick(void);
HAL_StatusTypeDef HAL_UARTEx_ReceiveToIdle_DMA(UART_HandleTypeDef *, uint8_t *, uint16_t);
HAL_StatusTypeDef HAL_UART_AbortReceive_IT(UART_HandleTypeDef *);
HAL_StatusTypeDef HAL_UART_AbortTransmit_IT(UART_HandleTypeDef *);
HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef *, const uint8_t *, uint16_t);
HAL_UART_RxEventTypeTypeDef HAL_UARTEx_GetRxEventType(UART_HandleTypeDef *);
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *, uint16_t);
void HAL_UART_AbortReceiveCpltCallback(UART_HandleTypeDef *);
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *);
void HAL_UART_ErrorCallback(UART_HandleTypeDef *);
void HAL_UART_AbortTransmitCpltCallback(UART_HandleTypeDef *);
#endif

#define DMA1_Stream0 (&model_dma_streams[0])

#define DMA1_Stream1 (&model_dma_streams[1])

#define DMA1_Stream2 (&model_dma_streams[2])

#define DMA1_Stream3 (&model_dma_streams[3])

#define DMA1_Stream4 (&model_dma_streams[4])

#define DMA1_Stream5 (&model_dma_streams[5])

#define DMA1_Stream6 (&model_dma_streams[6])

#define DMA1_Stream7 (&model_dma_streams[7])

#define DMA2_Stream0 (&model_dma_streams[8])

#define DMA2_Stream1 (&model_dma_streams[9])

#define DMA2_Stream2 (&model_dma_streams[10])

#define DMA2_Stream3 (&model_dma_streams[11])

#define DMA2_Stream4 (&model_dma_streams[12])

#define DMA2_Stream5 (&model_dma_streams[13])

#define DMA2_Stream6 (&model_dma_streams[14])

#define DMA2_Stream7 (&model_dma_streams[15])
