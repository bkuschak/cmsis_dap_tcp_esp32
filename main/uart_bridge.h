#ifndef UART_BRIDGE_H
#define UART_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

void uart_bridge_task(void* arg);
void uart_bridge_print_status(void);

#ifdef __cplusplus
}
#endif

#endif
