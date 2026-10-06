#include <stdio.h>
#include "uart_comm_port.h"
extern int model_last_cleared_irq;
extern uint32_t model_pending_clears;
uint32_t model_primask;
void model_clear_bit(volatile uint32_t *reg, uint32_t bit) { *reg &= ~bit; }
void model_bitband_clear(volatile uint32_t *reg, uint32_t bit) { *reg &= ~bit; }
void model_set_primask(uint32_t value) { model_primask = value; }
#define CHECK(e) do { if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); return 1; } } while (0)
int main(void)
{
    const IRQn_Type expected[] = {
        DMA1_Stream0_IRQn, DMA1_Stream1_IRQn, DMA1_Stream2_IRQn, DMA1_Stream3_IRQn,
        DMA1_Stream4_IRQn, DMA1_Stream5_IRQn, DMA1_Stream6_IRQn, DMA1_Stream7_IRQn,
        DMA2_Stream0_IRQn, DMA2_Stream1_IRQn, DMA2_Stream2_IRQn, DMA2_Stream3_IRQn,
        DMA2_Stream4_IRQn, DMA2_Stream5_IRQn, DMA2_Stream6_IRQn, DMA2_Stream7_IRQn
    };
    unsigned i;
    IRQn_Type irq = DMA1_Stream0_IRQn;
    DMA_Stream_TypeDef unknown = {0};
    DMA_HandleTypeDef dma = {.Instance = DMA2_Stream7};
    UART_HandleTypeDef uart = {.hdmatx = &dma};
    for (i = 0; i < 16; i++) CHECK(uart_port_dma_irq(&model_dma_streams[i], &irq) && irq == expected[i]);
    CHECK(!uart_port_dma_irq(NULL, &irq));
    CHECK(!uart_port_dma_irq(&unknown, &irq));
    CHECK(!uart_port_dma_irq(DMA1_Stream0, NULL));
    puts("PASS dma_irq_mapping (16 streams + invalid inputs)");
    dma.Instance->flags = 31U;
    uart_port_tx_clear_sources(&uart);
    CHECK(dma.Instance->flags == 0 && model_pending_clears == 1U && model_last_cleared_irq == DMA2_Stream7_IRQn);
    puts("PASS selected_tx_irq_clear");
    return 0;
}
