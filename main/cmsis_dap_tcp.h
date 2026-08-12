#ifndef CMSIS_DAP_TCP_H
#define CMSIS_DAP_TCP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include "DAP_gpio_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// The TLS handshake (ECDHE + certificate parsing) runs inline in this
// task and needs substantially more stack than the rest of the task ever
// used before -- a plain 4096 overflowed and crashed the task in testing.
#if CONFIG_ESP_TLS_ENABLED
#define CMSIS_DAP_TCP_TASK_STACK_SIZE 8192
#else
#define CMSIS_DAP_TCP_TASK_STACK_SIZE 4096
#endif
#define CMSIS_DAP_TCP_TASK_PRIORITY 5

struct cmsis_dap_tcp_config {
    int instance;   // Used for identification in log messages only.
    int port;
    int keepalive_timeout;     // seconds; 0 disables keepalive
    struct cmsis_dap_gpio_config gpio;
};

// Start the CMSIS-DAP TCP task. config must not be NULL, and must remain
// valid for the lifetime of the task.
BaseType_t cmsis_dap_tcp_start(const struct cmsis_dap_tcp_config *config,
        const char *task_name, TaskHandle_t *handle);

// Task that runs the TCP server and processes requests and responses.
void cmsis_dap_tcp_task(void* arg);

void cmsis_dap_print_status(FILE *out);

#ifdef __cplusplus
}
#endif

#endif  // CMSIS_DAP_TCP_H
