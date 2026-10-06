/* STM32F4 HAL donanim baglantisi. Uygulama uart_comm.h kullanir. */
#ifndef UART_COMM_PORT_H
#define UART_COMM_PORT_H
#include <stdbool.h>
#include "stm32f4xx_hal.h"
typedef struct { uint32_t tc_before, ndtr, tc_after; } uart_port_rx_sample_t;
bool uart_port_dma_irq(DMA_Stream_TypeDef *stream, IRQn_Type *out);
bool uart_port_validate(const UART_HandleTypeDef *uart);
bool uart_port_rx_active(const UART_HandleTypeDef *uart);
bool uart_port_rx_healthy(const UART_HandleTypeDef *uart);
bool uart_port_rx_stopped(const UART_HandleTypeDef *uart);
bool uart_port_tx_stopped(const UART_HandleTypeDef *uart);
/* Sample ve kaynak maskeleme: cagiran mevcut kritik bolumu tutar. */
uart_port_rx_sample_t uart_port_rx_sample(const UART_HandleTypeDef *uart);
void uart_port_rx_clear_tc(UART_HandleTypeDef *uart);
void uart_port_rx_mask_sources(UART_HandleTypeDef *uart);
void uart_port_rx_clear_errors(UART_HandleTypeDef *uart);
void uart_port_tx_disable_half_irq(UART_HandleTypeDef *uart);
void uart_port_tx_clear_sources(UART_HandleTypeDef *uart);
void uart_port_tx_mask_sources(UART_HandleTypeDef *uart);
#endif
