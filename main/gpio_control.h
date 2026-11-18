/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GPIO_CONTROL_H
#define GPIO_CONTROL_H

#include <stdio.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Call after nvs_flash_init(), before starting either console. No task is created.
esp_err_t gpio_control_init(void);

// Shared by the serial and socket console handlers; operations are serialized.
int gpio_control_command(FILE *out, int argc, char **argv);

// Internal coordination with ADC streaming. Initialize GPIO control before
// starting the consoles/ADC service. Never take this mutex inside a critical
// section. The pin query below requires this lock to be held.
esp_err_t gpio_control_lock(void);
void gpio_control_unlock(void);
bool gpio_control_pin_is_configured(int pin);

#ifdef __cplusplus
}
#endif
#endif
