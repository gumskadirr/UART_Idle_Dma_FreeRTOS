/* Istege bagli UART byte akisi -> mevcut cerceve protokolu baglantisi. */
#ifndef PROTOCOL_UART_H
#define PROTOCOL_UART_H
#include "protocol.h"
#include "uart_comm.h"

typedef struct {
    frame_parser_t parser;
    frame_handler_t on_frame;
    void *user;
    uint8_t validated;
} protocol_uart_t;

/* Scheduler/start oncesi bir kez. Context UART omru boyunca yasamali. */
void protocol_uart_init(protocol_uart_t *ctx, frame_handler_t handler, void *user);
uint32_t protocol_uart_on_rx(uart_comm_rx_event_t event, const uint8_t *data,
                            uint16_t len, void *user);
#endif
