#ifndef SOCKET_CONSOLE_H
#define SOCKET_CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct socket_console_config {
    int port;
    int keepalive_timeout;     // seconds; 0 disables keepalive
};

// Starts the socket console listener task. Supports multiple simultaneous
// client connections.
BaseType_t socket_console_start(const struct socket_console_config *config,
        const char *task_name, TaskHandle_t *handle);

#ifdef __cplusplus
}
#endif

#endif
