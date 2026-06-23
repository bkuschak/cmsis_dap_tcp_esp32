#ifndef UART_BRIDGE_H
#define UART_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

struct uart_bridge_config {
    int port;
    int keepalive_timeout;
    int uart_num;
    int txd_pin;
    int rxd_pin;
    int baud_rate;
    int data_bits;
    int parity;
    int stop_bits;
};

void uart_bridge_task(void* arg);

#ifdef __cplusplus
}
#endif

#endif
