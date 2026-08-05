#ifndef DAP_GPIO_CONFIG_H
#define DAP_GPIO_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

enum cmsis_dap_led_type {
    CMSIS_DAP_LED_NONE = 0,
    CMSIS_DAP_LED_STANDARD,
    CMSIS_DAP_LED_RGB,
};

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
    enum cmsis_dap_led_type led_type;
    // Only meaningful when led_type == CMSIS_DAP_LED_RGB.
    int led_rgb_r;
    int led_rgb_g;
    int led_rgb_b;
    int io_port_write_cycles;
    int delay_slow_cycles;
    int drive_strength;
};

extern __thread const struct cmsis_dap_gpio_config *cmsis_dap_gpio_config;

int cmsis_dap_gpio_config_conflicts(
        const struct cmsis_dap_gpio_config *a,
        const struct cmsis_dap_gpio_config *b);

#ifdef __cplusplus
}
#endif

#endif  // DAP_GPIO_CONFIG_H
