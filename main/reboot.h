#ifndef REBOOT_H
#define REBOOT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

static inline void reboot(void)
{
    fflush(stdout);
    fflush(stderr);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();      // Does not return.
}

#ifdef __cplusplus
}
#endif

#endif // REBOOT_H
