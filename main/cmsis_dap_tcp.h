#ifndef CMSIS_DAP_TCP_H
#define CMSIS_DAP_TCP_H

#ifdef __cplusplus
extern "C" {
#endif

//#define DEBUG_PRINTING

#ifdef DEBUG_PRINTING
#define LOG_DEBUG(...) \
{ \
    fprintf(stderr, "cmsis_dap_tcp: "); \
    fprintf(stderr, ##__VA_ARGS__); \
    fprintf(stderr, "\n"); \
}

#else
#define LOG_DEBUG(...) { }
#endif

#include "DAP_gpio_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CMSIS_DAP_TCP_TASK_STACK_SIZE 4096
#define CMSIS_DAP_TCP_TASK_PRIORITY 5

struct cmsis_dap_tcp_config {
    int port;
    int disable_keepalive;
    int keepalive_timeout;
    struct cmsis_dap_gpio_config gpio;
};

// Start the CMSIS-DAP TCP task. config must not be NULL, and must remain
// valid for the lifetime of the task.
BaseType_t cmsis_dap_tcp_start(const struct cmsis_dap_tcp_config *config,
        const char *task_name, TaskHandle_t *handle);

// Task that runs the TCP server and processes requests and responses.
void cmsis_dap_tcp_task(void* arg);

void cmsis_dap_print_status(void);

#ifdef __cplusplus
}
#endif

#endif  // CMSIS_DAP_TCP_H
