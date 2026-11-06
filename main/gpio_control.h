/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GPIO_CONTROL_H
#define GPIO_CONTROL_H

#include <stdio.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Call after nvs_flash_init(), before starting either console. No task is created.
esp_err_t gpio_control_init(void);

// Shared by the serial and socket console handlers; operations are serialized.
int gpio_control_command(FILE *out, int argc, char **argv);

#ifdef __cplusplus
}
#endif
#endif
