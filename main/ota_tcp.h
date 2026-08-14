#ifndef OTA_TCP_H
#define OTA_TCP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct ota_tcp_config {
    int port;
    int keepalive_timeout;         // seconds; 0 disables keepalive
};

// Start the OTA TCP task. Accepts one client at a time; streams a firmware
// image straight into flash via esp_ota_write().
BaseType_t ota_tcp_start(const struct ota_tcp_config *config,
        const char *task_name, TaskHandle_t *handle);

#ifdef __cplusplus
}
#endif

#endif
