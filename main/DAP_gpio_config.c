#include "sdkconfig.h"

#include "DAP_gpio_config.h"

__thread const struct cmsis_dap_gpio_config *cmsis_dap_gpio_config;

void cmsis_dap_gpio_config_init(struct cmsis_dap_gpio_config *gpio)
{
    gpio->swclk_tck = CONFIG_ESP_DAP_GPIO_SWCLK_TCK;
    gpio->swdio_tms = CONFIG_ESP_DAP_GPIO_SWDIO_TMS;
#ifdef CONFIG_ESP_DAP_JTAG_SUPPORTED
    gpio->tdi = CONFIG_ESP_DAP_GPIO_TDI;
    gpio->tdo = CONFIG_ESP_DAP_GPIO_TDO;
#else
    gpio->tdi = -1;
    gpio->tdo = -1;
#endif
#ifdef CONFIG_ESP_DAP_JTAG_NTRST_SUPPORTED
    gpio->ntrst = CONFIG_ESP_DAP_GPIO_NTRST;
#else
    gpio->ntrst = -1;
#endif
#ifdef CONFIG_ESP_DAP_NRESET_SUPPORTED
    gpio->nreset = CONFIG_ESP_DAP_GPIO_NRESET;
#else
    gpio->nreset = -1;
#endif
#ifdef CONFIG_ESP_DAP_LED_SUPPORTED
    gpio->led = CONFIG_ESP_DAP_GPIO_LED;
#ifdef CONFIG_ESP_DAP_LED_ACTIVE_HIGH
    gpio->led_active_high = 1;
#else
    gpio->led_active_high = 0;
#endif
#else
    gpio->led = -1;
    gpio->led_active_high = 0;
#endif
    gpio->io_port_write_cycles = CONFIG_ESP_DAP_IO_PORT_WRITE_CYCLES;
    gpio->delay_slow_cycles = CONFIG_ESP_DAP_DELAY_SLOW_CYCLES;
}

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
