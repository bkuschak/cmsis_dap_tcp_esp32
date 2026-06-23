#ifndef DAP_GPIO_CONFIG_H
#define DAP_GPIO_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

struct cmsis_dap_gpio_config {
    int swclk_tck;
    int swdio_tms;
    int tdi;
    int tdo;
    // Optional pins use -1 when they are not wired for this instance.
    int ntrst;
    int nreset;
    int led;
    int led_active_high;
    int io_port_write_cycles;
    int delay_slow_cycles;
};

extern __thread const struct cmsis_dap_gpio_config *cmsis_dap_gpio_config;

// Initialize GPIO config with build-time defaults. Callers can then override
// per-instance fields; optional pins are initialized to -1 when unavailable.
void cmsis_dap_gpio_config_init(struct cmsis_dap_gpio_config *gpio);

int cmsis_dap_gpio_config_conflicts(
        const struct cmsis_dap_gpio_config *a,
        const struct cmsis_dap_gpio_config *b);

#ifdef __cplusplus
}
#endif

#endif  // DAP_GPIO_CONFIG_H
