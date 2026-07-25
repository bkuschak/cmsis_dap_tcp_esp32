#include "DAP_gpio_config.h"

__thread const struct cmsis_dap_gpio_config *cmsis_dap_gpio_config;

static int gpio_pin_conflicts(int pin, const struct cmsis_dap_gpio_config *gpio)
{
    return pin >= 0 && (pin == gpio->swclk_tck || pin == gpio->swdio_tms ||
            pin == gpio->tdi || pin == gpio->tdo || pin == gpio->ntrst ||
            pin == gpio->nreset || pin == gpio->led);
}

int cmsis_dap_gpio_config_conflicts(
        const struct cmsis_dap_gpio_config *a,
        const struct cmsis_dap_gpio_config *b)
{
    return gpio_pin_conflicts(a->swclk_tck, b) ||
            gpio_pin_conflicts(a->swdio_tms, b) ||
            gpio_pin_conflicts(a->tdi, b) ||
            gpio_pin_conflicts(a->tdo, b) ||
            gpio_pin_conflicts(a->ntrst, b) ||
            gpio_pin_conflicts(a->nreset, b) ||
            gpio_pin_conflicts(a->led, b);
}
