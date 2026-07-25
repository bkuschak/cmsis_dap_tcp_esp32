#ifndef UART_BRIDGE_H
#define UART_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <driver/uart.h>

struct uart_bridge_config {
    int port;
    int keepalive_timeout;
    int uart_num;
    int txd_pin;
    int rxd_pin;
    int baud_rate;
    uart_word_length_t data_bits;
    uart_parity_t parity;
    uart_stop_bits_t stop_bits;
};

// Start the UART bridge task.
BaseType_t uart_bridge_start(const struct uart_bridge_config *config,
        const char *task_name, TaskHandle_t *handle);

#ifdef __cplusplus
}
#endif

#endif
