#ifndef UART_RTOS_TESTS_H
#define UART_RTOS_TESTS_H
#include "uart_comm.h"
#ifdef UART_COMM_TEST
extern const uart_comm_handlers_t uart_rtos_test_handlers;
void uart_rtos_tests_start(void);
void uart_rtos_test_done(void);
#ifdef UART_COMM_SERIAL_TEST
extern const uart_comm_handlers_t uart_serial_test_handlers;
void uart_serial_tests_start(void);
#endif
#endif
#endif
