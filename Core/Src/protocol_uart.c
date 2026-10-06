#include "protocol_uart.h"
#include <stddef.h>
#if defined(UART_COMM_TEST) && !defined(UART_HAL_MODEL)
#include "uart_comm_internal.h"
#endif

static void deliver(const frame_info_t *frame, void *user)
{
    protocol_uart_t *ctx = user;
    ctx->validated = 1U;
    if (ctx->on_frame == NULL) return;
#if defined(UART_COMM_TEST) && !defined(UART_HAL_MODEL)
    comm_test_frame_enter();
#endif
    ctx->on_frame(frame, ctx->user);
#if defined(UART_COMM_TEST) && !defined(UART_HAL_MODEL)
    comm_test_frame_exit();
#endif
}

void protocol_uart_init(protocol_uart_t *ctx, frame_handler_t handler, void *user)
{
    if (ctx == NULL) return;
    frame_parser_init(&ctx->parser);
    ctx->on_frame = handler;
    ctx->user = user;
    ctx->validated = 0U;
}

uint32_t protocol_uart_on_rx(uart_comm_rx_event_t event, const uint8_t *data,
                            uint16_t len, void *user)
{
    protocol_uart_t *ctx = user;
    if (ctx == NULL) return 0U;
    ctx->validated = 0U;
    switch (event) {
    case UART_COMM_RX_DATA:
        frame_parser_feed(&ctx->parser, data, len, deliver, ctx);
        break;
    case UART_COMM_RX_TIMEOUT:
        frame_parser_timeout(&ctx->parser, deliver, ctx);
        break;
    case UART_COMM_RX_RESET:
        frame_parser_discard(&ctx->parser);
        break;
    default: break;
    }
    return (ctx->parser.len ? UART_COMM_RX_PENDING : 0U) |
           (ctx->validated ? UART_COMM_RX_VALIDATED : 0U);
}
