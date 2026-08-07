/*
 * SPDX-FileCopyrightText: Brian Kuschak <bkuschak@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Console commands: registration, argument parsing, and handlers. Runs the
 * REPL over whichever transport CONFIG_ESP_CONSOLE_* selects.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include "sdkconfig.h"

#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "driver/uart.h"
#include "linenoise/linenoise.h"
#include "esp_wifi.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_types.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "commands.h"
#include "cmsis_dap_tcp.h"
#include "uart_bridge.h"

#ifdef CONFIG_ESP_ADC_STREAM_ENABLED
#include "adc_stream.h"
#include "soc/soc_caps.h"
#endif

// Owned by main.c.
extern void reboot(void);
extern volatile bool wifi_connected;
extern const char *wifi_ssid;

#define NVS_NAMESPACE           "wifi_config"
#define NVS_KEY_SSID            "ssid"
#define NVS_KEY_PASSWORD        "password"
#define NVS_KEY_AUTH_MODE       "auth_mode"
#define MAX_SSID_LEN            32
#define MAX_PASSWORD_LEN        64

static char stored_ssid[MAX_SSID_LEN] = {0};
static char stored_password[MAX_PASSWORD_LEN] = {0};
static wifi_auth_mode_t stored_auth_mode = WIFI_AUTH_WPA2_PSK;

static wifi_auth_mode_t parse_auth_mode(const char* auth_str)
{
    // Parse auth mode string to wifi_auth_mode_t.
    if (auth_str == NULL) {
        return WIFI_AUTH_WPA2_PSK;
    }

    if (strcmp(auth_str, "wep") == 0) {
        return WIFI_AUTH_WEP;
    } else if (strcmp(auth_str, "wpa") == 0) {
        return WIFI_AUTH_WPA_PSK;
    } else if (strcmp(auth_str, "wpa2") == 0) {
        return WIFI_AUTH_WPA2_PSK;
    } else if (strcmp(auth_str, "wpa3") == 0) {
        return WIFI_AUTH_WPA3_PSK;
    } else if (strcmp(auth_str, "open") == 0) {
        return WIFI_AUTH_OPEN;
    } else {
        return WIFI_AUTH_WPA2_PSK; // Default
    }
}

static esp_err_t save_wifi_credentials(const char* ssid, const char* password,
        wifi_auth_mode_t auth_mode)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_KEY_PASSWORD, password);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, NVS_KEY_AUTH_MODE, (uint8_t)auth_mode);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return err;
}

static esp_err_t load_wifi_credentials(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t ssid_len = MAX_SSID_LEN;
    size_t password_len = MAX_PASSWORD_LEN;
    uint8_t auth_mode;

    err = nvs_get_str(nvs_handle, NVS_KEY_SSID, stored_ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs_handle, NVS_KEY_PASSWORD, stored_password,
                &password_len);
    }
    if (err == ESP_OK) {
        err = nvs_get_u8(nvs_handle, NVS_KEY_AUTH_MODE, &auth_mode);
        if (err == ESP_OK) {
            stored_auth_mode = (wifi_auth_mode_t)auth_mode;
        }
    }

    nvs_close(nvs_handle);
    return err;
}

static esp_err_t clear_wifi_credentials(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    // Erase the entire namespace to clear all WiFi credentials.
    err = nvs_erase_all(nvs_handle);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return err;
}

bool commands_get_stored_wifi_credentials(const char **ssid,
        const char **password, wifi_auth_mode_t *auth_mode)
{
    if (load_wifi_credentials() != ESP_OK || strlen(stored_ssid) == 0)
        return false;

    *ssid = stored_ssid;
    *password = stored_password;
    *auth_mode = stored_auth_mode;
    return true;
}

// WiFi command argument structure.
static struct {
    struct arg_str *ssid;
    struct arg_str *password;
    struct arg_str *auth_mode;
    struct arg_end *end;
} wifi_args;

static int wifi_cmd_handler(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &wifi_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, wifi_args.end, argv[0]);
        printf("Usage: wifi \"<ssid>\" \"<password>\" [auth_mode]\n");
        printf("  auth_mode: open, wep, wpa, wpa2, wpa3 (default: wpa2)\n");
        return 1;
    }

    const char* ssid = wifi_args.ssid->sval[0];
    const char* password = wifi_args.password->sval[0];
    const char* auth_mode_str = wifi_args.auth_mode->count > 0 ?
        wifi_args.auth_mode->sval[0] : "wpa2";

    // Check for empty SSID. If empty, clear stored credentials.
    if (strlen(ssid) == 0) {
        printf("Empty SSID provided. Clearing WiFi credentials from flash.\n");
        esp_err_t err = clear_wifi_credentials();
        if (err == ESP_OK) {
            printf("WiFi credentials cleared successfully.\n");
            printf("Reboot required to use hardcoded CONFIG values.\n");
        } else {
            printf("Error clearing WiFi credentials: %s\n",
                    esp_err_to_name(err));
            return 1;
        }
        return 0;
    }

    // Validate input lengths.
    if (strlen(ssid) >= MAX_SSID_LEN) {
        printf("Error: SSID too long (max %d characters)\n", MAX_SSID_LEN - 1);
        return 1;
    }
    if (strlen(password) >= MAX_PASSWORD_LEN) {
        printf("Error: Password too long (max %d characters)\n",
                MAX_PASSWORD_LEN - 1);
        return 1;
    }

    wifi_auth_mode_t auth_mode = parse_auth_mode(auth_mode_str);

    printf("WiFi credentials received:\n");
    printf("  SSID: %s\n", ssid);
    printf("  Password: %s\n", password);
    printf("  Auth mode: %s\n", auth_mode_str);

    esp_err_t err = save_wifi_credentials(ssid, password, auth_mode);
    if (err == ESP_OK) {
        printf("WiFi credentials saved successfully.\n");
        printf("Reboot required to apply new settings.\n");
    } else {
        printf("Error saving WiFi credentials: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

#if defined(CONFIG_ESP_UART_BRIDGE_1_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_2_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_3_ENABLED)

// UART command argument structure.
static struct {
    struct arg_int *instance;
    struct arg_int *baud_rate;
    struct arg_int *data_bits;
    struct arg_str *parity;
    struct arg_int *stop_bits;
    struct arg_end *end;
} uart_args;

// Map a UART bridge instance number (1/2/3) to the physical UART peripheral
// number it's configured to use. Returns -1 if that instance doesn't exist
// or isn't enabled.
static int uart_bridge_instance_to_uart_num(int instance)
{
    switch (instance) {
#ifdef CONFIG_ESP_UART_BRIDGE_1_ENABLED
    case 1: return CONFIG_ESP_UART_BRIDGE_1_UART_NUM;
#endif
#ifdef CONFIG_ESP_UART_BRIDGE_2_ENABLED
    case 2: return CONFIG_ESP_UART_BRIDGE_2_UART_NUM;
#endif
#ifdef CONFIG_ESP_UART_BRIDGE_3_ENABLED
    case 3: return CONFIG_ESP_UART_BRIDGE_3_UART_NUM;
#endif
    default: return -1;
    }
}

static bool parse_uart_parity(const char* parity_str, uart_parity_t* out)
{
    if (strlen(parity_str) != 1)
        return false;

    switch (toupper((unsigned char)parity_str[0])) {
        case 'N': *out = UART_PARITY_DISABLE; break;
        case 'E': *out = UART_PARITY_EVEN;    break;
        case 'O': *out = UART_PARITY_ODD;     break;
        default:  return false;
    }
    return true;
}

static int uart_cmd_handler(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &uart_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, uart_args.end, argv[0]);
        printf("Usage: uart <instance> <baud_rate> <data_bits> <parity> "
                "<stop_bits>\n");
        printf("  instance: which UART bridge to configure (1, 2, or 3)\n");
        printf("  data_bits: 7 or 8\n");
        printf("  parity: n (none), e (even), o (odd) -- case-insensitive\n");
        printf("  stop_bits: 1 or 2\n");
        printf("  Use baud_rate 0 to clear stored settings and revert to "
                "CONFIG defaults.\n");
        return 1;
    }

    int instance = uart_args.instance->ival[0];
    int baud_rate = uart_args.baud_rate->ival[0];
    int data_bits = uart_args.data_bits->ival[0];
    const char* parity_str = uart_args.parity->sval[0];
    int stop_bits = uart_args.stop_bits->ival[0];

    int uart_num = uart_bridge_instance_to_uart_num(instance);
    if (uart_num < 0) {
        printf("Error: no enabled UART bridge instance %d.\n", instance);
        return 1;
    }

    // A baud rate of 0 clears the stored settings.
    if (baud_rate == 0) {
        printf("Clearing UART settings from flash.\n");
        esp_err_t err = uart_bridge_clear_config(uart_num);
        if (err == ESP_OK) {
            printf("UART settings cleared successfully.\n");
            printf("New connections will use CONFIG defaults.\n");
        } else {
            printf("Error clearing UART settings: %s\n", esp_err_to_name(err));
            return 1;
        }
        return 0;
    }

    if (baud_rate < 0) {
        printf("Error: baud rate must be positive.\n");
        return 1;
    }
    if (data_bits != 7 && data_bits != 8) {
        printf("Error: data bits must be 7 or 8.\n");
        return 1;
    }
    uart_parity_t parity;
    if (!parse_uart_parity(parity_str, &parity)) {
        printf("Error: parity must be n, e, or o.\n");
        return 1;
    }
    if (stop_bits != 1 && stop_bits != 2) {
        printf("Error: stop bits must be 1 or 2.\n");
        return 1;
    }

    printf("UART settings received:\n");
    printf("  Instance: %d\n", instance);
    printf("  UART number: %d\n", uart_num);
    printf("  Baud rate: %d\n", baud_rate);
    printf("  Data bits: %d\n", data_bits);
    printf("  Parity: %s\n", parity_str);
    printf("  Stop bits: %d\n", stop_bits);

    uart_word_length_t data_bits_enum =
            data_bits == 7 ? UART_DATA_7_BITS : UART_DATA_8_BITS;
    uart_stop_bits_t stop_bits_enum =
            stop_bits == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;

    esp_err_t err = uart_bridge_save_config(uart_num, baud_rate,
            data_bits_enum, parity, stop_bits_enum);
    if (err == ESP_OK) {
        printf("UART settings saved successfully.\n");
    } else {
        printf("Error saving UART settings: %s\n", esp_err_to_name(err));
        return 1;
    }

    err = uart_bridge_apply_live_config(uart_num, baud_rate, data_bits_enum,
            parity, stop_bits_enum);
    if (err == ESP_OK) {
        printf("Applied immediately to UART%d.\n", uart_num);
    } else {
        printf("Error applying settings immediately: %s\n",
                esp_err_to_name(err));
        printf("New connections will still use the saved settings.\n");
    }
    return 0;
}
#endif

#ifdef CONFIG_ESP_ADC_STREAM_ENABLED
// ADC stream command argument structure.
static struct {
    struct arg_int *gpio;
    struct arg_int *output_sps;
    struct arg_int *averaging;
    struct arg_str *format;
    struct arg_end *end;
} adc_args;

static bool parse_adc_format(const char* format_str, enum adc_stream_format* out)
{
    if (strcasecmp(format_str, "text") == 0) {
        *out = ADC_STREAM_FORMAT_TEXT;
        return true;
    }
    if (strcasecmp(format_str, "binary16") == 0 ||
            strcasecmp(format_str, "bin") == 0) {
        *out = ADC_STREAM_FORMAT_BINARY16;
        return true;
    }
    return false;
}

static int adc_cmd_handler(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &adc_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, adc_args.end, argv[0]);
        printf("Usage: adc <gpio> <output_sps> <averaging> <format>\n");
        printf("  averaging: power of 2 from 1 to 1024 (1 = no averaging)\n");
        printf("  format: text or binary16\n");
        printf("  Use output_sps 0 to clear stored settings and revert to "
                "CONFIG defaults.\n");
        return 1;
    }

    int gpio = adc_args.gpio->ival[0];
    int output_sps = adc_args.output_sps->ival[0];
    int averaging = adc_args.averaging->ival[0];
    const char* format_str = adc_args.format->sval[0];

    // An output rate of 0 clears the stored settings. (gpio can't double as
    // the sentinel -- GPIO0 is a valid, already-default pin.)
    if (output_sps == 0) {
        printf("Clearing ADC stream settings from flash.\n");
        esp_err_t err = adc_stream_clear_config();
        if (err == ESP_OK) {
            printf("ADC stream settings cleared successfully.\n");
            printf("New connections will use CONFIG defaults.\n");
        } else {
            printf("Error clearing ADC stream settings: %s\n",
                    esp_err_to_name(err));
            return 1;
        }
        return 0;
    }

    if (output_sps < 0) {
        printf("Error: output sample rate must be positive.\n");
        return 1;
    }
    if (averaging < 1 || averaging > 1024 ||
            (averaging & (averaging - 1)) != 0) {
        printf("Error: averaging must be a power of 2 between 1 and 1024.\n");
        return 1;
    }
    // Same raw-rate sanity check enforced at connection time in
    // adc_stream_task() -- fail fast here instead of only at next connect.
    uint32_t raw_sample_freq_hz = (uint32_t)output_sps * (uint32_t)averaging;
    if (raw_sample_freq_hz < SOC_ADC_SAMPLE_FREQ_THRES_LOW ||
            raw_sample_freq_hz > SOC_ADC_SAMPLE_FREQ_THRES_HIGH) {
        printf("Error: output rate (%d) x averaging (%d) = %" PRIu32
                " Hz raw ADC rate, outside the supported range (%d - %d "
                "Hz).\n", output_sps, averaging, raw_sample_freq_hz,
                SOC_ADC_SAMPLE_FREQ_THRES_LOW, SOC_ADC_SAMPLE_FREQ_THRES_HIGH);
        return 1;
    }
    enum adc_stream_format format;
    if (!parse_adc_format(format_str, &format)) {
        printf("Error: format must be 'text' or 'binary16'.\n");
        return 1;
    }

    printf("ADC stream settings received:\n");
    printf("  GPIO: %d\n", gpio);
    printf("  Output rate: %d Hz\n", output_sps);
    printf("  Averaging: %d\n", averaging);
    printf("  Format: %s\n", format_str);

    esp_err_t err = adc_stream_save_config(gpio, output_sps, averaging, format);
    if (err == ESP_OK) {
        printf("ADC stream settings saved successfully.\n");
        printf("New connections will use these settings.\n");
    } else {
        printf("Error saving ADC stream settings: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}
#endif

static int reboot_cmd_handler(int argc, char **argv)
{
    printf("Rebooting...\n");
    reboot();   // Does not return.
    return 0;
}

#ifdef CONFIG_LWIP_IPV6
static const char* ipv6_type_str(esp_ip6_addr_type_t type)
{
    switch (type) {
        case ESP_IP6_ADDR_IS_GLOBAL:            return "global";
        case ESP_IP6_ADDR_IS_LINK_LOCAL:        return "link-local";
        case ESP_IP6_ADDR_IS_UNIQUE_LOCAL:      return "unique-local";
        case ESP_IP6_ADDR_IS_SITE_LOCAL:        return "site-local";
        case ESP_IP6_ADDR_IS_IPV4_MAPPED_IPV6:  return "ipv4-mapped";
        case ESP_IP6_ADDR_IS_UNKNOWN:
        default:                                return "unknown";
    }
}
#endif

static int status_cmd_handler(int argc, char **argv)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    if (netif == NULL) {
        printf("WiFi interface not found.\n");
        return 0;
    }

    if(!wifi_connected) {
        printf("Not connected to WiFi SSID: '%s'\n", wifi_ssid);
    }
    else {
        int rssi = 0;
        esp_wifi_sta_get_rssi(&rssi);
        printf("Connected to WiFi SSID: '%s'. RSSI: %d dBm\n", wifi_ssid,
                rssi);

        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
            printf("Failed reading IP address.\n");
        }
        else {
            if (ip_info.ip.addr == 0) {
                printf("No IP address assigned.\n");
            }
            else {
                printf("IP address: " IPSTR "\n", IP2STR(&ip_info.ip));
            }
        }

#ifdef CONFIG_LWIP_IPV6
        esp_ip6_addr_t ip6_addrs[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
        int count = esp_netif_get_all_ip6(netif, ip6_addrs);

        if (count == 0) {
            printf("No IPv6 address assigned.\n");
        }
        for(int i=0; i<count; i++) {
            esp_ip6_addr_type_t ipv6_type =
                esp_netif_ip6_get_addr_type(&ip6_addrs[i]);

            printf("IPv6 address (%s): " IPV6STR "\n",
                    ipv6_type_str(ipv6_type), IPV62STR(ip6_addrs[i]));
        }
#endif
    }

    cmsis_dap_print_status();

#if defined(CONFIG_ESP_UART_BRIDGE_1_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_2_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_3_ENABLED)
    uart_bridge_print_status();
#endif

#ifdef CONFIG_ESP_ADC_STREAM_ENABLED
    adc_stream_print_status();
#endif
    return 0;
}

static int help_cmd_handler(int argc, char **argv)
{
    printf("Available commands:\n");
    printf("  help - Show this help message.\n");
    printf("  wifi \"<ssid>\" \"<password>\" [auth_mode] - Configure WiFi "
           "credentials.\n");
#if defined(CONFIG_ESP_UART_BRIDGE_1_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_2_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_3_ENABLED)
    printf("  uart <instance> <baud_rate> <data_bits> <parity> <stop_bits> - "
           "Configure UART bridge settings.\n");
#endif
#ifdef CONFIG_ESP_ADC_STREAM_ENABLED
    printf("  adc <gpio> <output_sps> <averaging> <format> - Configure "
           "ADC streaming settings.\n");
#endif
    printf("  reboot - Restart the device.\n");
    printf("  status - Report network status.\n");
    return 0;
}

void commands_init(void)
{
    // Initialize console.
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "esp32> ";
    repl_config.max_cmdline_length = 256;

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || \
    defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hw_config =
        ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config,
                &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t hw_config =
        ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw_config, &repl_config,
                &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config,
                &repl_config, &repl));
#else
#error "Unsupported console type!"
#endif

    // The REPL setup above registers a per-keystroke "hints" callback that
    // fires once the typed buffer's length matches a registered command
    // name, rendering that command's .hint via ANSI escape codes. None of
    // our commands set a .hint, so it has nothing useful to show, and on
    // some terminals it shows up as garbage characters instead. Disable it.
    linenoiseSetHintsCallback(NULL);

    // Register commands.
    const esp_console_cmd_t help_cmd = {
        .command = "help",
        .help = "Show available commands",
        .hint = NULL,
        .func = &help_cmd_handler,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&help_cmd));

    const esp_console_cmd_t reboot_cmd = {
        .command = "reboot",
        .help = "Restart the device",
        .hint = NULL,
        .func = &reboot_cmd_handler,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&reboot_cmd));

    const esp_console_cmd_t status_cmd = {
        .command = "status",
        .help = "Show device status",
        .hint = NULL,
        .func = &status_cmd_handler,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&status_cmd));

    wifi_args.ssid = arg_str1(NULL, NULL, "<ssid>", "WiFi network SSID");
    wifi_args.password =
        arg_str1(NULL, NULL, "<password>", "WiFi network password");
    wifi_args.auth_mode =
        arg_str0(NULL, NULL, "[auth_mode]", "Authentication mode: open, wep, "
                "wpa, wpa2, wpa3");
    wifi_args.end = arg_end(3);

    const esp_console_cmd_t wifi_cmd = {
        .command = "wifi",
        .help = "Configure WiFi credentials",
        .hint = NULL,
        .func = &wifi_cmd_handler,
        .argtable = &wifi_args
    };
    printf("Enabling console commands.\n");
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_cmd));

#if defined(CONFIG_ESP_UART_BRIDGE_1_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_2_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_3_ENABLED)
    uart_args.instance = arg_int1(NULL, NULL, "<instance>",
            "Which UART bridge to configure (1, 2, or 3)");
    uart_args.baud_rate = arg_int1(NULL, NULL, "<baud_rate>",
            "UART baud rate (0 clears stored settings)");
    uart_args.data_bits = arg_int1(NULL, NULL, "<data_bits>",
            "Data bits: 7 or 8");
    uart_args.parity = arg_str1(NULL, NULL, "<parity>",
            "Parity: n, e, o (case-insensitive)");
    uart_args.stop_bits = arg_int1(NULL, NULL, "<stop_bits>",
            "Stop bits: 1 or 2");
    uart_args.end = arg_end(5);

    const esp_console_cmd_t uart_cmd = {
        .command = "uart",
        .help = "Configure UART bridge settings",
        .hint = NULL,
        .func = &uart_cmd_handler,
        .argtable = &uart_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&uart_cmd));
#endif

#ifdef CONFIG_ESP_ADC_STREAM_ENABLED
    adc_args.gpio = arg_int1(NULL, NULL, "<gpio>", "ADC1 input GPIO");
    adc_args.output_sps = arg_int1(NULL, NULL, "<output_sps>",
            "Output sample rate in Hz (0 clears stored settings)");
    adc_args.averaging = arg_int1(NULL, NULL, "<averaging>",
            "Averaging count: power of 2, 1-1024");
    adc_args.format = arg_str1(NULL, NULL, "<format>",
            "Output format: text or binary16");
    adc_args.end = arg_end(4);

    const esp_console_cmd_t adc_cmd = {
        .command = "adc",
        .help = "Configure ADC streaming settings",
        .hint = NULL,
        .func = &adc_cmd_handler,
        .argtable = &adc_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&adc_cmd));
#endif

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
