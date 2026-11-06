/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_private/esp_gpio_reserve.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "soc/soc_caps.h"
#include "soc/uart_pins.h"
#include "gpio_control.h"

#ifdef CONFIG_ESP_ADC_STREAM_ENABLED
#include "esp_adc/adc_continuous.h"
#endif

#define GPIO_NVS_NAMESPACE "gpio_control"
#define GPIO_CONFIG_VERSION 1

enum control_mode { CONTROL_UNUSED, CONTROL_INPUT, CONTROL_PUSH_PULL, CONTROL_OPEN_DRAIN };
enum control_pull { CONTROL_FLOATING, CONTROL_PULL_UP, CONTROL_PULL_DOWN };

// Explicit bytes keep the on-flash format independent of enum sizes/padding.
struct pin_config {
    uint8_t version;
    uint8_t mode;
    uint8_t pull;
    uint8_t default_level;
};

static struct pin_config pin_configs[GPIO_NUM_MAX];
static int output_levels[GPIO_NUM_MAX];
static SemaphoreHandle_t control_mutex;
static uint64_t allowed_pins;
static uint64_t adc_pins;
static bool ready;

static inline bool uart_default_pin(int uart, int pin)
{
    switch (uart) {
    case 0: return pin == U0TXD_GPIO_NUM || pin == U0RXD_GPIO_NUM;
#ifdef U1TXD_GPIO_NUM
    case 1: return pin == U1TXD_GPIO_NUM || pin == U1RXD_GPIO_NUM;
#endif
#ifdef U2TXD_GPIO_NUM
    case 2: return pin == U2TXD_GPIO_NUM || pin == U2RXD_GPIO_NUM;
#endif
    default: return false;
    }
}

static bool interface_pin(int pin)
{
    static const int dap_pins[] = {
        -1,
#if defined(CONFIG_ESP_DAP_1_SWD_SUPPORTED) || defined(CONFIG_ESP_DAP_1_JTAG_SUPPORTED)
        CONFIG_ESP_DAP_1_GPIO_SWCLK_TCK, CONFIG_ESP_DAP_1_GPIO_SWDIO_TMS,
#endif
#ifdef CONFIG_ESP_DAP_1_JTAG_SUPPORTED
        CONFIG_ESP_DAP_1_GPIO_TDI, CONFIG_ESP_DAP_1_GPIO_TDO,
#ifdef CONFIG_ESP_DAP_1_JTAG_NTRST_SUPPORTED
        CONFIG_ESP_DAP_1_GPIO_NTRST,
#endif
#endif
#ifdef CONFIG_ESP_DAP_1_NRESET_SUPPORTED
        CONFIG_ESP_DAP_1_GPIO_NRESET,
#endif
#if defined(CONFIG_ESP_DAP_1_LED_STANDARD) || defined(CONFIG_ESP_DAP_1_LED_RGB)
        CONFIG_ESP_DAP_1_GPIO_LED,
#endif
#ifdef CONFIG_ESP_DAP_2_ENABLED
#if defined(CONFIG_ESP_DAP_2_SWD_SUPPORTED) || defined(CONFIG_ESP_DAP_2_JTAG_SUPPORTED)
        CONFIG_ESP_DAP_2_GPIO_SWCLK_TCK, CONFIG_ESP_DAP_2_GPIO_SWDIO_TMS,
#endif
#ifdef CONFIG_ESP_DAP_2_JTAG_SUPPORTED
        CONFIG_ESP_DAP_2_GPIO_TDI, CONFIG_ESP_DAP_2_GPIO_TDO,
#ifdef CONFIG_ESP_DAP_2_JTAG_NTRST_SUPPORTED
        CONFIG_ESP_DAP_2_GPIO_NTRST,
#endif
#endif
#ifdef CONFIG_ESP_DAP_2_NRESET_SUPPORTED
        CONFIG_ESP_DAP_2_GPIO_NRESET,
#endif
#if defined(CONFIG_ESP_DAP_2_LED_STANDARD) || defined(CONFIG_ESP_DAP_2_LED_RGB)
        CONFIG_ESP_DAP_2_GPIO_LED,
#endif
#endif
#ifdef CONFIG_ESP_DAP_3_ENABLED
#if defined(CONFIG_ESP_DAP_3_SWD_SUPPORTED) || defined(CONFIG_ESP_DAP_3_JTAG_SUPPORTED)
        CONFIG_ESP_DAP_3_GPIO_SWCLK_TCK, CONFIG_ESP_DAP_3_GPIO_SWDIO_TMS,
#endif
#ifdef CONFIG_ESP_DAP_3_JTAG_SUPPORTED
        CONFIG_ESP_DAP_3_GPIO_TDI, CONFIG_ESP_DAP_3_GPIO_TDO,
#ifdef CONFIG_ESP_DAP_3_JTAG_NTRST_SUPPORTED
        CONFIG_ESP_DAP_3_GPIO_NTRST,
#endif
#endif
#ifdef CONFIG_ESP_DAP_3_NRESET_SUPPORTED
        CONFIG_ESP_DAP_3_GPIO_NRESET,
#endif
#if defined(CONFIG_ESP_DAP_3_LED_STANDARD) || defined(CONFIG_ESP_DAP_3_LED_RGB)
        CONFIG_ESP_DAP_3_GPIO_LED,
#endif
#endif
    };
    for (size_t i = 0; i < sizeof(dap_pins) / sizeof(dap_pins[0]); i++) {
        if (pin == dap_pins[i])
            return true;
    }

#ifdef CONFIG_ESP_UART_BRIDGE_1_ENABLED
#ifdef CONFIG_ESP_UART_BRIDGE_1_REMAP_PINS
    if (pin == CONFIG_ESP_UART_BRIDGE_1_TXD_PIN || pin == CONFIG_ESP_UART_BRIDGE_1_RXD_PIN)
        return true;
#else
    if (uart_default_pin(CONFIG_ESP_UART_BRIDGE_1_UART_NUM, pin))
        return true;
#endif
#endif
#ifdef CONFIG_ESP_UART_BRIDGE_2_ENABLED
#ifdef CONFIG_ESP_UART_BRIDGE_2_REMAP_PINS
    if (pin == CONFIG_ESP_UART_BRIDGE_2_TXD_PIN || pin == CONFIG_ESP_UART_BRIDGE_2_RXD_PIN)
        return true;
#else
    if (uart_default_pin(CONFIG_ESP_UART_BRIDGE_2_UART_NUM, pin))
        return true;
#endif
#endif
#ifdef CONFIG_ESP_UART_BRIDGE_3_ENABLED
#ifdef CONFIG_ESP_UART_BRIDGE_3_REMAP_PINS
    if (pin == CONFIG_ESP_UART_BRIDGE_3_TXD_PIN || pin == CONFIG_ESP_UART_BRIDGE_3_RXD_PIN)
        return true;
#else
    if (uart_default_pin(CONFIG_ESP_UART_BRIDGE_3_UART_NUM, pin))
        return true;
#endif
#endif
#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
#ifdef CONFIG_ESP_CONSOLE_UART_CUSTOM
    if (pin == CONFIG_ESP_CONSOLE_UART_TX_GPIO || pin == CONFIG_ESP_CONSOLE_UART_RX_GPIO)
        return true;
#else
    if (uart_default_pin(CONFIG_ESP_CONSOLE_UART_NUM, pin))
        return true;
#endif
#endif
#ifdef CONFIG_ESP_CONSOLE_SECONDARY_UART
    if (uart_default_pin(0, pin))
        return true;
#endif
    return false;
}

static const char *unavailable_reason(int pin)
{
    if (pin < 0 || pin >= GPIO_NUM_MAX || pin >= 64 || !GPIO_IS_VALID_GPIO(pin))
        return "invalid GPIO";
    uint64_t bit = UINT64_C(1) << pin;
    if (!(allowed_pins & bit))
        return "not in the Kconfig allow-list";
    if (esp_gpio_is_reserved(bit))
        return "reserved by ESP-IDF (for example flash/PSRAM)";
    if (interface_pin(pin))
        return "used by a configured DAP, LED, UART or console interface";
    // Keep native USB and chip-debug JTAG available, independently of the console.
#if CONFIG_IDF_TARGET_ESP32C3
    if (pin == 18 || pin == 19 || (pin >= 4 && pin <= 7))
#elif CONFIG_IDF_TARGET_ESP32S3
    if (pin == 19 || pin == 20 || (pin >= 39 && pin <= 42))
#elif CONFIG_IDF_TARGET_ESP32C6
    if (pin == 12 || pin == 13 || (pin >= 4 && pin <= 7))
#else
#error "GPIO control needs native USB/JTAG pin definitions for this target"
#endif
        return "native USB or chip-debug JTAG pin";
    if (adc_pins & bit)
        return "ADC1 pin (ADC streaming is enabled)";
    return NULL;
}

static bool valid_config(int pin, const struct pin_config *config)
{
    if (config->version != GPIO_CONFIG_VERSION || config->default_level > 1)
        return false;
    if (config->mode == CONTROL_INPUT) {
        return config->pull <= CONTROL_PULL_DOWN &&
            config->default_level == 0 &&
            (config->pull == CONTROL_FLOATING || GPIO_IS_VALID_OUTPUT_GPIO(pin));
    }
    return (config->mode == CONTROL_PUSH_PULL || config->mode == CONTROL_OPEN_DRAIN) &&
        config->pull == CONTROL_FLOATING && GPIO_IS_VALID_OUTPUT_GPIO(pin);
}

static esp_err_t apply_config(int pin, const struct pin_config *config, int level)
{
    gpio_config_t hw = {
        .pin_bit_mask = UINT64_C(1) << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = config->pull == CONTROL_PULL_UP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = config->pull == CONTROL_PULL_DOWN ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (config->mode == CONTROL_PUSH_PULL || config->mode == CONTROL_OPEN_DRAIN) {
        // Load the latch before enabling output, including after reboot.
        esp_err_t err = gpio_set_direction(pin, GPIO_MODE_INPUT);
        if (err != ESP_OK)
            return err;
        err = gpio_set_level(pin, level);
        if (err != ESP_OK)
            return err;
        hw.mode = config->mode == CONTROL_OPEN_DRAIN ? GPIO_MODE_INPUT_OUTPUT_OD : GPIO_MODE_INPUT_OUTPUT;
    }
    return gpio_config(&hw);
}

static esp_err_t store_config(int pin, const struct pin_config *config)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(GPIO_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return err;
    char key[8];
    snprintf(key, sizeof(key), "p%d", pin);
    if (config->mode == CONTROL_UNUSED) {
        err = nvs_erase_key(handle, key);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            err = ESP_OK;
    } else {
        err = nvs_set_blob(handle, key, config, sizeof(*config));
    }
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t change_config(int pin, const struct pin_config *config)
{
    esp_err_t err = apply_config(pin, config, config->default_level);
    if (err == ESP_OK)
        err = store_config(pin, config);
    if (err != ESP_OK) {
        esp_err_t rollback = apply_config(pin, &pin_configs[pin], output_levels[pin]);
        if (rollback != ESP_OK)
            fprintf(stderr, "GPIO%d: rollback failed: %s\n", pin, esp_err_to_name(rollback));
        return err;
    }
    pin_configs[pin] = *config;
    output_levels[pin] = config->default_level;
    return ESP_OK;
}

static bool parse_number(const char *text, int maximum, int *value)
{
    if (!text || !isdigit((unsigned char)text[0]))
        return false;
    errno = 0;
    char *end;
    long number = strtol(text, &end, 10);
    if (errno || *end || number < 0 || number > maximum)
        return false;
    *value = (int)number;
    return true;
}

static bool parse_allowed_pins(void)
{
    const char *next = CONFIG_ESP_GPIO_CONTROL_ALLOWED_PINS;
    uint64_t mask = 0;
    while (isspace((unsigned char)*next))
        next++;
    while (*next) {
        if (!isdigit((unsigned char)*next))
            return false;
        char *end;
        errno = 0;
        long pin = strtol(next, &end, 10);
        if (errno || pin < 0 || pin >= GPIO_NUM_MAX || pin >= 64 || !GPIO_IS_VALID_GPIO(pin))
            return false;
        mask |= UINT64_C(1) << pin;
        next = end;
        while (isspace((unsigned char)*next))
            next++;
        if (!*next)
            break;
        if (*next++ != ',')
            return false;
        while (isspace((unsigned char)*next))
            next++;
        if (!*next)
            return false;
    }
    allowed_pins = mask;
    return true;
}

esp_err_t gpio_control_init(void)
{
    if (control_mutex != NULL)
        return ESP_ERR_INVALID_STATE;
    if (!parse_allowed_pins()) {
        fprintf(stderr, "GPIO control: invalid Kconfig allow-list; control disabled.\n");
        return ESP_ERR_INVALID_ARG;
    }
    control_mutex = xSemaphoreCreateMutex();
    if (control_mutex == NULL)
        return ESP_ERR_NO_MEM;

#ifdef CONFIG_ESP_ADC_STREAM_ENABLED
    // ADC GPIO can change at each connection; reserve the entire ADC1 set.
    for (int channel = 0; channel < SOC_ADC_CHANNEL_NUM(0); channel++) {
        int pin;
        if (adc_continuous_channel_to_io(ADC_UNIT_1, channel, &pin) == ESP_OK && pin >= 0 && pin < 64)
            adc_pins |= UINT64_C(1) << pin;
    }
#endif

    nvs_handle_t handle;
    esp_err_t err = nvs_open(GPIO_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ready = true;
        return ESP_OK;
    }
    if (err != ESP_OK)
        return err;
    for (int pin = 0; pin < GPIO_NUM_MAX && pin < 64; pin++) {
        char key[8];
        snprintf(key, sizeof(key), "p%d", pin);
        struct pin_config config;
        size_t size = sizeof(config);
        err = nvs_get_blob(handle, key, &config, &size);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            continue;
        if (err != ESP_OK || size != sizeof(config) ||
                unavailable_reason(pin) != NULL || !valid_config(pin, &config)) {
            fprintf(stderr, "GPIO%d: ignoring invalid or unavailable saved configuration.\n", pin);
            continue;
        }
        err = apply_config(pin, &config, config.default_level);
        if (err == ESP_OK) {
            pin_configs[pin] = config;
            output_levels[pin] = config.default_level;
        } else {
            fprintf(stderr, "GPIO%d: restore failed: %s\n", pin, esp_err_to_name(err));
        }
    }
    nvs_close(handle);
    ready = true;
    return ESP_OK;
}

static void print_usage(FILE *out)
{
    fprintf(out, "Usage:\n"
            "  gpio list\n"
            "  gpio config <pin> input <floating|up|down>\n"
            "  gpio config <pin> <push-pull|open-drain> <0|1>\n"
            "  gpio read <pin>\n"
            "  gpio write <pin> <0|1>\n"
            "  gpio clear <pin>\n"
            "config saves settings and the boot level; write changes only the live level.\n");
}

static int run_command(FILE *out, int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "list") == 0) {
        if (allowed_pins == 0)
            fprintf(out, "No GPIOs allowed. Set ESP_GPIO_CONTROL_ALLOWED_PINS in menuconfig.\n");
        for (int pin = 0; pin < GPIO_NUM_MAX && pin < 64; pin++) {
            if (!(allowed_pins & (UINT64_C(1) << pin)))
                continue;
            const char *reason = unavailable_reason(pin);
            if (reason != NULL) {
                fprintf(out, "GPIO%d: unavailable: %s\n", pin, reason);
                continue;
            }
            const struct pin_config *config = &pin_configs[pin];
            if (config->mode == CONTROL_UNUSED) {
                fprintf(out, "GPIO%d: available, unconfigured\n", pin);
            } else if (config->mode == CONTROL_INPUT) {
                const char *pulls[] = { "floating", "up", "down" };
                fprintf(out, "GPIO%d: input %s, level=%d\n", pin, pulls[config->pull], gpio_get_level(pin));
            } else {
                fprintf(out, "GPIO%d: %s, boot=%u, latch=%d, level=%d\n", pin,
                        config->mode == CONTROL_OPEN_DRAIN ? "open-drain" : "push-pull",
                        (unsigned)config->default_level, output_levels[pin], gpio_get_level(pin));
            }
        }
        return 0;
    }

    bool configure = argc == 5 && strcmp(argv[1], "config") == 0;
    bool read = argc == 3 && strcmp(argv[1], "read") == 0;
    bool write = argc == 4 && strcmp(argv[1], "write") == 0;
    bool clear = argc == 3 && strcmp(argv[1], "clear") == 0;
    if (!configure && !read && !write && !clear) {
        print_usage(out);
        return 1;
    }
    int pin;
    if (!parse_number(argv[2], GPIO_NUM_MAX - 1, &pin)) {
        fprintf(out, "Error: invalid GPIO number.\n");
        return 1;
    }
    const char *reason = unavailable_reason(pin);
    if (reason != NULL) {
        fprintf(out, "Error: GPIO%d: %s.\n", pin, reason);
        return 1;
    }

    esp_err_t err = ESP_OK;
    if (configure) {
        struct pin_config config = { .version = GPIO_CONFIG_VERSION };
        if (strcmp(argv[3], "input") == 0) {
            config.mode = CONTROL_INPUT;
            if (strcmp(argv[4], "floating") == 0)
                config.pull = CONTROL_FLOATING;
            else if (strcmp(argv[4], "up") == 0)
                config.pull = CONTROL_PULL_UP;
            else if (strcmp(argv[4], "down") == 0)
                config.pull = CONTROL_PULL_DOWN;
            else {
                print_usage(out);
                return 1;
            }
        } else {
            int level;
            if (strcmp(argv[3], "push-pull") == 0)
                config.mode = CONTROL_PUSH_PULL;
            else if (strcmp(argv[3], "open-drain") == 0)
                config.mode = CONTROL_OPEN_DRAIN;
            else {
                print_usage(out);
                return 1;
            }
            if (!parse_number(argv[4], 1, &level)) {
                fprintf(out, "Error: output level must be 0 or 1.\n");
                return 1;
            }
            config.default_level = level;
        }
        if (!valid_config(pin, &config)) {
            fprintf(out, "Error: GPIO%d does not support this mode or pull resistor.\n", pin);
            return 1;
        }
        err = change_config(pin, &config);
    } else if (clear) {
        const struct pin_config unused = {0};
        err = change_config(pin, &unused);
    } else if (pin_configs[pin].mode == CONTROL_UNUSED) {
        fprintf(out, "Error: configure GPIO%d first.\n", pin);
        return 1;
    } else if (read) {
        fprintf(out, "GPIO%d: %d\n", pin, gpio_get_level(pin));
        return 0;
    } else if (write) {
        int level;
        if (pin_configs[pin].mode == CONTROL_INPUT || !parse_number(argv[3], 1, &level)) {
            fprintf(out, "Error: write requires an output and a level of 0 or 1.\n");
            return 1;
        }
        err = gpio_set_level(pin, level);
        if (err == ESP_OK)
            output_levels[pin] = level;
    }
    if (err != ESP_OK) {
        fprintf(out, "Error: GPIO%d: %s.\n", pin, esp_err_to_name(err));
        return 1;
    }
    fprintf(out, "GPIO%d: %s.\n", pin, write ? "live level updated (not saved)" :
            clear ? "saved configuration removed; floating input" : "configuration applied and saved");
    return 0;
}

int gpio_control_command(FILE *out, int argc, char **argv)
{
    if (!ready) {
        fprintf(out, "Error: GPIO control is not initialized.\n");
        return 1;
    }
    xSemaphoreTake(control_mutex, portMAX_DELAY);
    int result = run_command(out, argc, argv);
    xSemaphoreGive(control_mutex);
    return result;
}
