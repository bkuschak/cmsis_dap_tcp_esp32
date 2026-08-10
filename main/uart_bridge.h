#ifndef UART_BRIDGE_H
#define UART_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <driver/uart.h>
#include "esp_err.h"

struct uart_bridge_config {
    int instance; // Used for identification in log messages only.
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

void uart_bridge_print_status(FILE *out);

// Persist UART line settings to flash for the given UART number. Applied to
// new TCP client connections; does not affect an already-open connection.
// Call uart_bridge_apply_live_config() too if the change should also take
// effect immediately.
esp_err_t uart_bridge_save_config(int uart_num, int baud_rate,
        uart_word_length_t data_bits, uart_parity_t parity,
        uart_stop_bits_t stop_bits);

// Apply UART line settings to the running peripheral immediately, including
// while a client is already connected. Safe to call from any task once
// that UART's driver is installed (i.e. once its bridge task has started).
// Does not persist anything -- pair with uart_bridge_save_config() if the
// change should also survive a reboot. Bytes in flight at the moment of
// the change may be garbled, same as changing settings on real hardware.
esp_err_t uart_bridge_apply_live_config(int uart_num, int baud_rate,
        uart_word_length_t data_bits, uart_parity_t parity,
        uart_stop_bits_t stop_bits);

// Erase persisted UART line settings for the given UART number. Subsequent
// TCP client connections fall back to the CONFIG defaults.
esp_err_t uart_bridge_clear_config(int uart_num);

#ifdef __cplusplus
}
#endif

#endif
