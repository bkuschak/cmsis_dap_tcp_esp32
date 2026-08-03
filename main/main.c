/*
 * SPDX-FileCopyrightText: Brian Kuschak <bkuschak@gmail.com>
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP32 app supporting the OpenOCD 'CMSIS-DAP over TCP/IP' protocol.
 *
 * Allows an ESP32 to act as a JTAG/SWD programmer for a target device such as
 * an ARM microcontroller. The host connects to this programmer using OpenOCD.
 *
 * Refer to the OpenOCD file src/jtag/drivers/cmsis_dap_tcp.c for the host
 * implementation.
 *
 * Build using the ESP32-IDF tools. Tested on ESP32-C6. To support another
 * board / CPU, dap/DAP_config.h will need to be modified.
 *
 * The CMSIS-DAP commands and responses are sent over a TCP socket rather than
 * USB. Use the OpenOCD cmsis_dap_tcp driver to connect. OpenOCD must be
 * configured with settings like these:
 *
 *     adapter driver cmsis-dap
 *     cmsis-dap backend tcp
 *     cmsis-dap tcp host 192.168.1.4
 *     cmsis-dap tcp port 4441
 *
 * Programming a target can then be done using something like this:
 *
 *     openocd --search tcl \
 *         -f tcl/interface/cmsis_dap_tcp.cfg \
 *         -f tcl/target/stm32f1x.cfg \
 *         -c "transport select swd" \
 *         -c "adapter speed 2000" \
 *         -c "program firmware.elf verify reset exit"
 *
 * Status:
 * - Supports SWD, JTAG, NRESET, TRST.
 * - Provides a UART-to-TCP/IP bridge for the target's serial console.
 * - SWO trace port currently unsupported.
 *
 * Some parts of this code were adapted from:
 *     esp-idf/examples/get-started/hello_world
 *     esp-idf/examples/wifi/getting_started/station
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include "sdkconfig.h"

#include "cpu_usage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_types.h"
#include "esp_netif_net_stack.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "lwip/err.h"
#include "lwip/dhcp6.h"
#include "lwip/prot/dhcp6.h"
#include "lwip/sys.h"
#include "nvs_flash.h"
#include "nvs.h"

#ifdef CONFIG_ESP_WIFI_CONSOLE_COMMANDS
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "driver/uart.h"
#include "linenoise/linenoise.h"
#endif

#include "DAP.h"
#include "cmsis_dap_tcp.h"
#include "uart_bridge.h"

#ifdef CONFIG_ESP_ADC_STREAM_ENABLED
#include "adc_stream.h"
#include "soc/soc_caps.h"
#endif

#if defined(CONFIG_ESP_DAP_1_LED_RGB) || \
    defined(CONFIG_ESP_DAP_2_LED_RGB) || \
    defined(CONFIG_ESP_DAP_3_LED_RGB)
#include "ws2812_led.h"
#endif

#ifdef CONFIG_ESP_WIFI_CONSOLE_COMMANDS
#define NVS_NAMESPACE           "wifi_config"
#define NVS_KEY_SSID            "ssid"
#define NVS_KEY_PASSWORD        "password"
#define NVS_KEY_AUTH_MODE       "auth_mode"
#define MAX_SSID_LEN            32
#define MAX_PASSWORD_LEN        64
#endif

#if CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define EXAMPLE_H2E_IDENTIFIER ""

#elif CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID

#elif CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_BOTH
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif

#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN

#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP

#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK

#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK

#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK

#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK

#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK

#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

/* Use an event group to signal two WiFi related events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries
 */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static uint8_t mac_addr[6];
static char mac_addr_str[16];
static int wifi_retry_num;
static EventGroupHandle_t wifi_event_group;
static bool cmsis_dap_tcp_initialized;
static volatile bool wifi_connected;
static esp_netif_t *sta_netif;

static const char* wifi_ssid = CONFIG_ESP_WIFI_SSID;
static const char* wifi_password = CONFIG_ESP_WIFI_PASSWORD;
static wifi_auth_mode_t wifi_auth_mode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD;

#ifdef CONFIG_ESP_WIFI_CONSOLE_COMMANDS
static char stored_ssid[MAX_SSID_LEN] = {0};
static char stored_password[MAX_PASSWORD_LEN] = {0};
static wifi_auth_mode_t stored_auth_mode = WIFI_AUTH_WPA2_PSK;
#endif

static void reboot(void)
{
    fflush(stdout);
    fflush(stderr);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();      // Does not return.
}

#ifdef CONFIG_ESP_WIFI_CONSOLE_COMMANDS
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

static void commands_init(void)
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
#endif

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            printf("Attempting to connect to WiFi SSID: '%s'\n", wifi_ssid);
            esp_wifi_connect();
        }
        else if (event_id == WIFI_EVENT_STA_CONNECTED) {
            int rssi = 0;
            esp_wifi_sta_get_rssi(&rssi);
            printf("Connected to WiFi SSID: '%s'. RSSI: %d dBm\n", wifi_ssid,
                    rssi);
            wifi_connected = true;
        }
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_connected = false;
            if (cmsis_dap_tcp_initialized) {
                /* If connection is lost after we have initialized the server,
                 * any connected sockets and the server socket have been lost.
                 * The server socket must be reinitialized. Just reboot to
                 * reinitialize everything.
                 */
                printf("Lost connection to WiFi SSID: '%s'. Rebooting...\n",
                        wifi_ssid);
                reboot();       // Does not return.
            }
            if (wifi_retry_num < CONFIG_ESP_MAXIMUM_RETRY) {
                printf("Retrying connection to WiFi SSID: '%s'\n", wifi_ssid);
                esp_wifi_connect();
                wifi_retry_num++;
            }
            else {
                printf("Failed to connect to WiFi SSID: '%s'.\n", wifi_ssid);
                xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            }
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        printf("IP address: " IPSTR "\n", IP2STR(&event->ip_info.ip));
        wifi_retry_num = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
#ifdef CONFIG_LWIP_IPV6
    else if (event_base == IP_EVENT && event_id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t* event = (ip_event_got_ip6_t*) event_data;
        esp_ip6_addr_type_t ipv6_type =
            esp_netif_ip6_get_addr_type(&event->ip6_info.ip);
        printf("IPv6 address (%s): " IPV6STR "\n",
                (ipv6_type == ESP_IP6_ADDR_IS_LINK_LOCAL) ? "link-local" :
                "global", IPV62STR(event->ip6_info.ip));
    }
#endif
}

int wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
                WIFI_EVENT,
                ESP_EVENT_ANY_ID,
                &event_handler,
                NULL,
                &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
                IP_EVENT,
                IP_EVENT_STA_GOT_IP,
                &event_handler,
                NULL,
                &instance_got_ip));
#ifdef CONFIG_LWIP_IPV6
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
                IP_EVENT,
                IP_EVENT_GOT_IP6,
                &event_handler,
                NULL,
                &instance_got_ip));
#endif

#ifdef CONFIG_ESP_WIFI_CONSOLE_COMMANDS
    // Try to load WiFi credentials from NVS, fall back to config values.
    if (load_wifi_credentials() == ESP_OK && strlen(stored_ssid) > 0) {
        wifi_ssid = stored_ssid;
        wifi_password = stored_password;
        wifi_auth_mode = stored_auth_mode;
        printf("Using WiFi credentials from flash.\n");
    } else {
        printf("Using WiFi credentials from hardcoded CONFIG.\n");
    }
#else
    printf("Using WiFi credentials from hardcoded CONFIG.\n");
#endif

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = wifi_auth_mode,
            .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
            .sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
        },
    };

    // Copy SSID and password (need to handle const char* to uint8_t[]
    // conversion).
    strlcpy((char*)wifi_config.sta.ssid, wifi_ssid,
            sizeof(wifi_config.sta.ssid));
    strlcpy((char*)wifi_config.sta.password, wifi_password,
            sizeof(wifi_config.sta.password));
    if(wifi_auth_mode == WIFI_AUTH_OPEN) {
        wifi_config.sta.pmf_cfg.capable = true;
        wifi_config.sta.pmf_cfg.required = false;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Wait until either the connection is established (WIFI_CONNECTED_BIT)
     * or connection failed for the maximum number of re-tries (WIFI_FAIL_BIT).
     * The bits are set by event_handler() above.
     */
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
#ifdef CONFIG_ESP_DAP_DISABLE_WIFI_POWER_SAVE
        // Disable power-save to improve WiFi performance.
        // https://github.com/espressif/arduino-esp32/issues/1484
        printf("Disabling WiFi power savings to improve performance.\n");
        esp_wifi_set_ps(WIFI_PS_NONE);
#endif
#ifdef CONFIG_LWIP_IPV6
        // Trigger IPv6 SLAAC auto-configuration, after IPv4 is up.
        esp_err_t err = esp_netif_create_ip6_linklocal(sta_netif);
        if (err != ESP_OK)
            perror("Failed to create IPv6 link local address");
#endif
        return 0;   // Success.
    }
    return -1;      // Failure.
}

static BaseType_t cmsis_dap_tcp_task_start(void)
{
    // Configure one CMSIS-DAP-TCP task. Static: must remain valid for the
    // task's lifetime, per cmsis_dap_tcp_start()'s contract.
    static struct cmsis_dap_tcp_config config = {
        .instance = 1,
        .port = CONFIG_ESP_DAP_1_TCP_PORT,
#ifdef CONFIG_ESP_DAP_TCP_USE_KEEPALIVE
        .disable_keepalive = false,
        .keepalive_timeout = CONFIG_ESP_DAP_TCP_KEEPALIVE_TIMEOUT,
#else
        .disable_keepalive = true,
        .keepalive_timeout = 0,
#endif
        .gpio = {
#if defined(CONFIG_ESP_DAP_1_JTAG_SUPPORTED) || defined(CONFIG_ESP_DAP_1_SWD_SUPPORTED)
            .swclk_tck = CONFIG_ESP_DAP_1_GPIO_SWCLK_TCK,
            .swdio_tms = CONFIG_ESP_DAP_1_GPIO_SWDIO_TMS,
#else
            .swclk_tck = -1,
            .swdio_tms = -1,
#endif
#ifdef CONFIG_ESP_DAP_1_JTAG_SUPPORTED
            .tdi = CONFIG_ESP_DAP_1_GPIO_TDI,
            .tdo = CONFIG_ESP_DAP_1_GPIO_TDO,
#else
            .tdi = -1,
            .tdo = -1,
#endif
#if defined(CONFIG_ESP_DAP_1_JTAG_SUPPORTED) && defined(CONFIG_ESP_DAP_1_JTAG_NTRST_SUPPORTED)
            .ntrst = CONFIG_ESP_DAP_1_GPIO_NTRST,
#else
            .ntrst = -1,
#endif
#ifdef CONFIG_ESP_DAP_1_NRESET_SUPPORTED
            .nreset = CONFIG_ESP_DAP_1_GPIO_NRESET,
#else
            .nreset = -1,
#endif
#if defined(CONFIG_ESP_DAP_1_LED_STANDARD) || defined(CONFIG_ESP_DAP_1_LED_RGB)
            .led = CONFIG_ESP_DAP_1_GPIO_LED,
#else
            .led = -1,
#endif
#ifdef CONFIG_ESP_DAP_1_LED_ACTIVE_HIGH
            .led_active_high = true,
#else
            .led_active_high = false,
#endif
#if defined(CONFIG_ESP_DAP_1_LED_RGB)
            .led_type = CMSIS_DAP_LED_RGB,
            .led_rgb_r = CONFIG_ESP_DAP_1_LED_RGB_INTENSITY_R,
            .led_rgb_g = CONFIG_ESP_DAP_1_LED_RGB_INTENSITY_G,
            .led_rgb_b = CONFIG_ESP_DAP_1_LED_RGB_INTENSITY_B,
#elif defined(CONFIG_ESP_DAP_1_LED_STANDARD)
            .led_type = CMSIS_DAP_LED_STANDARD,
#else
            .led_type = CMSIS_DAP_LED_NONE,
#endif
            .io_port_write_cycles = CONFIG_ESP_DAP_IO_PORT_WRITE_CYCLES,
            .delay_slow_cycles = CONFIG_ESP_DAP_DELAY_SLOW_CYCLES,
            .drive_strength = CONFIG_ESP_DAP_1_DRIVE_STRENGTH,
        },
    };

    BaseType_t ret = cmsis_dap_tcp_start(&config, "cmsis_dap_tcp_task", NULL);
    if(ret == pdPASS)
        cmsis_dap_tcp_initialized = true;
    return ret;
}

#ifdef CONFIG_ESP_DAP_2_ENABLED
static BaseType_t cmsis_dap_tcp_2_task_start(void)
{
    // Configure the second CMSIS-DAP-TCP task. Static: must remain valid for
    // the task's lifetime, per cmsis_dap_tcp_start()'s contract.
    static struct cmsis_dap_tcp_config config = {
        .instance = 2,
        .port = CONFIG_ESP_DAP_2_TCP_PORT,
#ifdef CONFIG_ESP_DAP_TCP_USE_KEEPALIVE
        .disable_keepalive = false,
        .keepalive_timeout = CONFIG_ESP_DAP_TCP_KEEPALIVE_TIMEOUT,
#else
        .disable_keepalive = true,
        .keepalive_timeout = 0,
#endif
        .gpio = {
#if defined(CONFIG_ESP_DAP_2_JTAG_SUPPORTED) || defined(CONFIG_ESP_DAP_2_SWD_SUPPORTED)
            .swclk_tck = CONFIG_ESP_DAP_2_GPIO_SWCLK_TCK,
            .swdio_tms = CONFIG_ESP_DAP_2_GPIO_SWDIO_TMS,
#else
            .swclk_tck = -1,
            .swdio_tms = -1,
#endif
#ifdef CONFIG_ESP_DAP_2_JTAG_SUPPORTED
            .tdi = CONFIG_ESP_DAP_2_GPIO_TDI,
            .tdo = CONFIG_ESP_DAP_2_GPIO_TDO,
#else
            .tdi = -1,
            .tdo = -1,
#endif
#if defined(CONFIG_ESP_DAP_2_JTAG_SUPPORTED) && defined(CONFIG_ESP_DAP_2_JTAG_NTRST_SUPPORTED)
            .ntrst = CONFIG_ESP_DAP_2_GPIO_NTRST,
#else
            .ntrst = -1,
#endif
#ifdef CONFIG_ESP_DAP_2_NRESET_SUPPORTED
            .nreset = CONFIG_ESP_DAP_2_GPIO_NRESET,
#else
            .nreset = -1,
#endif
#if defined(CONFIG_ESP_DAP_2_LED_STANDARD) || defined(CONFIG_ESP_DAP_2_LED_RGB)
            .led = CONFIG_ESP_DAP_2_GPIO_LED,
#else
            .led = -1,
#endif
#ifdef CONFIG_ESP_DAP_2_LED_ACTIVE_HIGH
            .led_active_high = true,
#else
            .led_active_high = false,
#endif
#if defined(CONFIG_ESP_DAP_2_LED_RGB)
            .led_type = CMSIS_DAP_LED_RGB,
            .led_rgb_r = CONFIG_ESP_DAP_2_LED_RGB_INTENSITY_R,
            .led_rgb_g = CONFIG_ESP_DAP_2_LED_RGB_INTENSITY_G,
            .led_rgb_b = CONFIG_ESP_DAP_2_LED_RGB_INTENSITY_B,
#elif defined(CONFIG_ESP_DAP_2_LED_STANDARD)
            .led_type = CMSIS_DAP_LED_STANDARD,
#else
            .led_type = CMSIS_DAP_LED_NONE,
#endif
            .io_port_write_cycles = CONFIG_ESP_DAP_IO_PORT_WRITE_CYCLES,
            .delay_slow_cycles = CONFIG_ESP_DAP_DELAY_SLOW_CYCLES,
            .drive_strength = CONFIG_ESP_DAP_2_DRIVE_STRENGTH,
        },
    };

    BaseType_t ret = cmsis_dap_tcp_start(&config, "cmsis_dap_tcp_task_2", NULL);
    if(ret == pdPASS)
        cmsis_dap_tcp_initialized = true;
    return ret;
}
#endif

#ifdef CONFIG_ESP_DAP_3_ENABLED
static BaseType_t cmsis_dap_tcp_3_task_start(void)
{
    // Configure the third CMSIS-DAP-TCP task. Static: must remain valid for
    // the task's lifetime, per cmsis_dap_tcp_start()'s contract.
    static struct cmsis_dap_tcp_config config = {
        .instance = 3,
        .port = CONFIG_ESP_DAP_3_TCP_PORT,
#ifdef CONFIG_ESP_DAP_TCP_USE_KEEPALIVE
        .disable_keepalive = false,
        .keepalive_timeout = CONFIG_ESP_DAP_TCP_KEEPALIVE_TIMEOUT,
#else
        .disable_keepalive = true,
        .keepalive_timeout = 0,
#endif
        .gpio = {
#if defined(CONFIG_ESP_DAP_3_JTAG_SUPPORTED) || defined(CONFIG_ESP_DAP_3_SWD_SUPPORTED)
            .swclk_tck = CONFIG_ESP_DAP_3_GPIO_SWCLK_TCK,
            .swdio_tms = CONFIG_ESP_DAP_3_GPIO_SWDIO_TMS,
#else
            .swclk_tck = -1,
            .swdio_tms = -1,
#endif
#ifdef CONFIG_ESP_DAP_3_JTAG_SUPPORTED
            .tdi = CONFIG_ESP_DAP_3_GPIO_TDI,
            .tdo = CONFIG_ESP_DAP_3_GPIO_TDO,
#else
            .tdi = -1,
            .tdo = -1,
#endif
#if defined(CONFIG_ESP_DAP_3_JTAG_SUPPORTED) && defined(CONFIG_ESP_DAP_3_JTAG_NTRST_SUPPORTED)
            .ntrst = CONFIG_ESP_DAP_3_GPIO_NTRST,
#else
            .ntrst = -1,
#endif
#ifdef CONFIG_ESP_DAP_3_NRESET_SUPPORTED
            .nreset = CONFIG_ESP_DAP_3_GPIO_NRESET,
#else
            .nreset = -1,
#endif
#if defined(CONFIG_ESP_DAP_3_LED_STANDARD) || defined(CONFIG_ESP_DAP_3_LED_RGB)
            .led = CONFIG_ESP_DAP_3_GPIO_LED,
#else
            .led = -1,
#endif
#ifdef CONFIG_ESP_DAP_3_LED_ACTIVE_HIGH
            .led_active_high = true,
#else
            .led_active_high = false,
#endif
#if defined(CONFIG_ESP_DAP_3_LED_RGB)
            .led_type = CMSIS_DAP_LED_RGB,
            .led_rgb_r = CONFIG_ESP_DAP_3_LED_RGB_INTENSITY_R,
            .led_rgb_g = CONFIG_ESP_DAP_3_LED_RGB_INTENSITY_G,
            .led_rgb_b = CONFIG_ESP_DAP_3_LED_RGB_INTENSITY_B,
#elif defined(CONFIG_ESP_DAP_3_LED_STANDARD)
            .led_type = CMSIS_DAP_LED_STANDARD,
#else
            .led_type = CMSIS_DAP_LED_NONE,
#endif
            .io_port_write_cycles = CONFIG_ESP_DAP_IO_PORT_WRITE_CYCLES,
            .delay_slow_cycles = CONFIG_ESP_DAP_DELAY_SLOW_CYCLES,
            .drive_strength = CONFIG_ESP_DAP_3_DRIVE_STRENGTH,
        },
    };

    BaseType_t ret = cmsis_dap_tcp_start(&config, "cmsis_dap_tcp_task_3", NULL);
    if(ret == pdPASS)
        cmsis_dap_tcp_initialized = true;
    return ret;
}
#endif

#ifdef CONFIG_ESP_UART_BRIDGE_1_ENABLED
static BaseType_t uart_bridge_task_start(void)
{
    // Configure one UART bridge task.
    static const struct uart_bridge_config config = {
        .instance   = 1,
        .port       = CONFIG_ESP_UART_BRIDGE_1_TCP_PORT,
        .uart_num   = CONFIG_ESP_UART_BRIDGE_1_UART_NUM,
#ifdef CONFIG_ESP_UART_BRIDGE_1_USE_KEEPALIVE
        .keepalive_timeout = CONFIG_ESP_UART_BRIDGE_1_KEEPALIVE_TIMEOUT,
#else
        .keepalive_timeout = 0,
#endif
#ifdef CONFIG_ESP_UART_BRIDGE_1_REMAP_PINS
        .txd_pin    = CONFIG_ESP_UART_BRIDGE_1_TXD_PIN,
        .rxd_pin    = CONFIG_ESP_UART_BRIDGE_1_RXD_PIN,
#else
        .txd_pin    = UART_PIN_NO_CHANGE,
        .rxd_pin    = UART_PIN_NO_CHANGE,
#endif
        .baud_rate  = CONFIG_ESP_UART_BRIDGE_1_BAUD_RATE,
#if defined(CONFIG_ESP_UART_BRIDGE_1_PARITY_NONE)
        .parity     = UART_PARITY_DISABLE,
#elif defined(CONFIG_ESP_UART_BRIDGE_1_PARITY_EVEN)
        .parity     = UART_PARITY_EVEN,
#elif defined(CONFIG_ESP_UART_BRIDGE_1_PARITY_ODD)
        .parity     = UART_PARITY_ODD,
#else
#error "Invalid setting for CONFIG_ESP_UART_BRIDGE_1_PARITY."
#endif

#if CONFIG_ESP_UART_BRIDGE_1_DATA_BITS == 7
        .data_bits  = UART_DATA_7_BITS,
#elif CONFIG_ESP_UART_BRIDGE_1_DATA_BITS == 8
        .data_bits  = UART_DATA_8_BITS,
#else
#error "Invalid setting for CONFIG_ESP_UART_BRIDGE_1_DATA_BITS."
#endif

#if CONFIG_ESP_UART_BRIDGE_1_STOP_BITS == 1
        .stop_bits  = UART_STOP_BITS_1,
#elif CONFIG_ESP_UART_BRIDGE_1_STOP_BITS == 2
        .stop_bits  = UART_STOP_BITS_2,
#else
#error "Invalid setting for CONFIG_ESP_UART_BRIDGE_1_STOP_BITS."
#endif
    };

    return uart_bridge_start(&config, "uart_bridge_task", NULL);
}
#endif

#ifdef CONFIG_ESP_UART_BRIDGE_2_ENABLED
static BaseType_t uart_bridge_2_task_start(void)
{
    // Configure the second UART bridge task.
    static const struct uart_bridge_config config = {
        .instance   = 2,
        .port       = CONFIG_ESP_UART_BRIDGE_2_TCP_PORT,
        .uart_num   = CONFIG_ESP_UART_BRIDGE_2_UART_NUM,
#ifdef CONFIG_ESP_UART_BRIDGE_2_USE_KEEPALIVE
        .keepalive_timeout = CONFIG_ESP_UART_BRIDGE_2_KEEPALIVE_TIMEOUT,
#else
        .keepalive_timeout = 0,
#endif
#ifdef CONFIG_ESP_UART_BRIDGE_2_REMAP_PINS
        .txd_pin    = CONFIG_ESP_UART_BRIDGE_2_TXD_PIN,
        .rxd_pin    = CONFIG_ESP_UART_BRIDGE_2_RXD_PIN,
#else
        .txd_pin    = UART_PIN_NO_CHANGE,
        .rxd_pin    = UART_PIN_NO_CHANGE,
#endif
        .baud_rate  = CONFIG_ESP_UART_BRIDGE_2_BAUD_RATE,
#if defined(CONFIG_ESP_UART_BRIDGE_2_PARITY_NONE)
        .parity     = UART_PARITY_DISABLE,
#elif defined(CONFIG_ESP_UART_BRIDGE_2_PARITY_EVEN)
        .parity     = UART_PARITY_EVEN,
#elif defined(CONFIG_ESP_UART_BRIDGE_2_PARITY_ODD)
        .parity     = UART_PARITY_ODD,
#else
#error "Invalid setting for CONFIG_ESP_UART_BRIDGE_2_PARITY."
#endif

#if CONFIG_ESP_UART_BRIDGE_2_DATA_BITS == 7
        .data_bits  = UART_DATA_7_BITS,
#elif CONFIG_ESP_UART_BRIDGE_2_DATA_BITS == 8
        .data_bits  = UART_DATA_8_BITS,
#else
#error "Invalid setting for CONFIG_ESP_UART_BRIDGE_2_DATA_BITS."
#endif

#if CONFIG_ESP_UART_BRIDGE_2_STOP_BITS == 1
        .stop_bits  = UART_STOP_BITS_1,
#elif CONFIG_ESP_UART_BRIDGE_2_STOP_BITS == 2
        .stop_bits  = UART_STOP_BITS_2,
#else
#error "Invalid setting for CONFIG_ESP_UART_BRIDGE_2_STOP_BITS."
#endif
    };

    return uart_bridge_start(&config, "uart_bridge_task_2", NULL);
}
#endif

#ifdef CONFIG_ESP_UART_BRIDGE_3_ENABLED
static BaseType_t uart_bridge_3_task_start(void)
{
    // Configure the third UART bridge task.
    static const struct uart_bridge_config config = {
        .instance   = 3,
        .port       = CONFIG_ESP_UART_BRIDGE_3_TCP_PORT,
        .uart_num   = CONFIG_ESP_UART_BRIDGE_3_UART_NUM,
#ifdef CONFIG_ESP_UART_BRIDGE_3_USE_KEEPALIVE
        .keepalive_timeout = CONFIG_ESP_UART_BRIDGE_3_KEEPALIVE_TIMEOUT,
#else
        .keepalive_timeout = 0,
#endif
#ifdef CONFIG_ESP_UART_BRIDGE_3_REMAP_PINS
        .txd_pin    = CONFIG_ESP_UART_BRIDGE_3_TXD_PIN,
        .rxd_pin    = CONFIG_ESP_UART_BRIDGE_3_RXD_PIN,
#else
        .txd_pin    = UART_PIN_NO_CHANGE,
        .rxd_pin    = UART_PIN_NO_CHANGE,
#endif
        .baud_rate  = CONFIG_ESP_UART_BRIDGE_3_BAUD_RATE,
#if defined(CONFIG_ESP_UART_BRIDGE_3_PARITY_NONE)
        .parity     = UART_PARITY_DISABLE,
#elif defined(CONFIG_ESP_UART_BRIDGE_3_PARITY_EVEN)
        .parity     = UART_PARITY_EVEN,
#elif defined(CONFIG_ESP_UART_BRIDGE_3_PARITY_ODD)
        .parity     = UART_PARITY_ODD,
#else
#error "Invalid setting for CONFIG_ESP_UART_BRIDGE_3_PARITY."
#endif

#if CONFIG_ESP_UART_BRIDGE_3_DATA_BITS == 7
        .data_bits  = UART_DATA_7_BITS,
#elif CONFIG_ESP_UART_BRIDGE_3_DATA_BITS == 8
        .data_bits  = UART_DATA_8_BITS,
#else
#error "Invalid setting for CONFIG_ESP_UART_BRIDGE_3_DATA_BITS."
#endif

#if CONFIG_ESP_UART_BRIDGE_3_STOP_BITS == 1
        .stop_bits  = UART_STOP_BITS_1,
#elif CONFIG_ESP_UART_BRIDGE_3_STOP_BITS == 2
        .stop_bits  = UART_STOP_BITS_2,
#else
#error "Invalid setting for CONFIG_ESP_UART_BRIDGE_3_STOP_BITS."
#endif
    };

    return uart_bridge_start(&config, "uart_bridge_task_3", NULL);
}
#endif

#ifdef CONFIG_ESP_ADC_STREAM_ENABLED
static BaseType_t adc_stream_task_start(void)
{
    static const struct adc_stream_config config = {
        .port               = CONFIG_ESP_ADC_STREAM_TCP_PORT,
#ifdef CONFIG_ESP_ADC_STREAM_USE_KEEPALIVE
        .keepalive_timeout  = CONFIG_ESP_ADC_STREAM_KEEPALIVE_TIMEOUT,
#else
        .keepalive_timeout  = 0,
#endif
        .gpio               = CONFIG_ESP_ADC_STREAM_GPIO,
        .sample_rate_hz     = CONFIG_ESP_ADC_STREAM_SAMPLE_RATE_HZ,
        .averaging_count    = CONFIG_ESP_ADC_STREAM_AVERAGING_COUNT,
#if defined(CONFIG_ESP_ADC_STREAM_FORMAT_TEXT)
        .format             = ADC_STREAM_FORMAT_TEXT,
#elif defined(CONFIG_ESP_ADC_STREAM_FORMAT_BINARY16)
        .format             = ADC_STREAM_FORMAT_BINARY16,
#else
#error "Invalid setting for CONFIG_ESP_ADC_STREAM_FORMAT."
#endif
        .correction_curve_x1000000 = CONFIG_ESP_ADC_STREAM_CORRECTION_CURVE_X1000000,
        .correction_gain_x1000  = CONFIG_ESP_ADC_STREAM_CORRECTION_GAIN_X1000,
        .correction_offset_mv   = CONFIG_ESP_ADC_STREAM_CORRECTION_OFFSET_MV,
    };

    return adc_stream_start(&config, "adc_stream_task", NULL);
}
#endif

void app_main(void)
{
    // DAP_Setup() runs in cmsis_dap_tcp_task(), which owns the task-local DAP
    // state used while processing CMSIS-DAP requests.
    printf("CMSIS-DAP TCP running on ESP32\n");
    printf("ESP-IDF version: %s\n", IDF_VER);

    // Print chip information.
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    printf("Hardware version: %s with %d CPU core(s), %s%s%s%s, ",
        CONFIG_IDF_TARGET,
        chip_info.cores,
        (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
        (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
        (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
        (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 "
            "(Zigbee/Thread)" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    printf("silicon revision v%d.%d, ", major_rev, minor_rev);
    if(esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        printf("Get flash size failed");
        return;
    }
    printf("%" PRIu32 "MB %s flash\n", flash_size / (uint32_t)(1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" :
           "external");
    printf("Minimum free heap size: %" PRIu32 " bytes\n",
            esp_get_minimum_free_heap_size());

    // MAC address is unique for every ESP32 device. Use it as a UID
    // when reporing data.
    esp_read_mac(mac_addr, ESP_MAC_WIFI_STA);
    snprintf(mac_addr_str, sizeof(mac_addr_str), "%02X%02X%02X%02X%02X%02X",
           mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4],
           mac_addr[5]);
    uint32_t serial_number = 0;
    serial_number |= mac_addr[2]; serial_number <<= 8;
    serial_number |= mac_addr[3]; serial_number <<= 8;
    serial_number |= mac_addr[4]; serial_number <<= 8;
    serial_number |= mac_addr[5];
    printf("MAC address: %s\n", mac_addr_str);

    // Initialize NVS.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

#ifdef CONFIG_ESP_WIFI_CONSOLE_COMMANDS
    commands_init();
#endif

    /* Initialize WiFi and connect to AP. If unable to connect after retries,
     * then force a reboot in an attempt to recover.
     */
    if (wifi_init() != 0) {
        printf("Restarting due to WiFi connection failures.\n");
        reboot();
    }

    if(cmsis_dap_tcp_task_start() != pdPASS) {
        printf("Failed to start CMSIS-DAP-TCP task.\n");
    }

#ifdef CONFIG_ESP_DAP_2_ENABLED
    if(cmsis_dap_tcp_2_task_start() != pdPASS) {
        printf("Failed to start second CMSIS-DAP-TCP task.\n");
    }
#endif

#ifdef CONFIG_ESP_DAP_3_ENABLED
    if(cmsis_dap_tcp_3_task_start() != pdPASS) {
        printf("Failed to start third CMSIS-DAP-TCP task.\n");
    }
#endif

#ifdef CONFIG_ESP_UART_BRIDGE_1_ENABLED
    if(uart_bridge_task_start() != pdPASS) {
        printf("Failed to start UART bridge task.\n");
    }
#endif

#ifdef CONFIG_ESP_UART_BRIDGE_2_ENABLED
    if(uart_bridge_2_task_start() != pdPASS) {
        printf("Failed to start second UART bridge task.\n");
    }
#endif

#ifdef CONFIG_ESP_UART_BRIDGE_3_ENABLED
    if(uart_bridge_3_task_start() != pdPASS) {
        printf("Failed to start third UART bridge task.\n");
    }
#endif

#ifdef CONFIG_ESP_ADC_STREAM_ENABLED
    if(adc_stream_task_start() != pdPASS) {
        printf("Failed to start ADC stream task.\n");
    }
#endif

#ifdef CONFIG_ESP_PRINT_CPU_USAGE
    xTaskCreatePinnedToCore(cpu_usage_task, "cpu_usage", 4096, NULL,
            CPU_USAGE_TASK_PRIO, NULL, tskNO_AFFINITY);
#endif
}
