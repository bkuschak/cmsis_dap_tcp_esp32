#ifndef UART_BRIDGE_H
#define UART_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <driver/uart.h>
#include "esp_err.h"

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

void uart_bridge_print_status(void);

// Persist UART line settings to flash for the given UART number. Applied to
// new TCP client connections; does not affect an already-open connection.
esp_err_t uart_bridge_save_config(int uart_num, int baud_rate,
        uart_word_length_t data_bits, uart_parity_t parity,
        uart_stop_bits_t stop_bits);

// Erase persisted UART line settings for the given UART number. Subsequent
// TCP client connections fall back to the CONFIG defaults.
esp_err_t uart_bridge_clear_config(int uart_num);

#ifdef __cplusplus
}
#endif

#endif
