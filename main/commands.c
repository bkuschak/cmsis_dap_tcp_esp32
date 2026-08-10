/*
 * SPDX-FileCopyrightText: Brian Kuschak <bkuschak@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Console commands: argument parsing, handlers, and dispatch for both the
 * serial console and socket console connections.
 *
 * Serial console registers the handlers once, at boot, via the ordinary
 * esp_console_cmd_register()/esp_console_start_repl() path. Socket can't
 * do that (esp_console doesn't support multiple instances), so socket
 * commands are handled separately.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include "sdkconfig.h"

#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "driver/uart.h"
#include "linenoise/linenoise.h"

#include "commands.h"
#include "cmsis_dap_tcp.h"
#include "uart_bridge.h"
#include "reboot.h"

#ifdef CONFIG_ESP_ADC_STREAM_ENABLED
#include "adc_stream.h"
#include "soc/soc_caps.h"
#endif

#ifdef CONFIG_ESP_DAP_MANAGE_WIFI
#include "esp_wifi_types_generic.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_types.h"
#include "wifi.h"
#endif

#ifdef CONFIG_ESP_DAP_SOCKET_CONSOLE_ENABLED
#include <errno.h>
#include <sys/socket.h>
#include "esp_linenoise.h"
#endif

#define MAX_CMD_LINE_ARGS       32

// Shared so both consoles show the same prompt.
#define COMMANDS_PROMPT "esp32> "

// Per-caller state (own argtable instances, no locking needed): one
// persistent instance for serial, one per TCP connection for socket.
enum command_transport {
    COMMAND_TRANSPORT_SERIAL,
    COMMAND_TRANSPORT_SOCKET,
};

struct command_context {
    FILE *out;
    enum command_transport transport;

#ifdef CONFIG_ESP_DAP_MANAGE_WIFI
    struct {
        struct arg_str *ssid;
        struct arg_str *password;
        struct arg_str *auth_mode;
        struct arg_end *end;
    } wifi_args;
#endif

#if defined(CONFIG_ESP_UART_BRIDGE_1_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_2_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_3_ENABLED)
    struct {
        struct arg_int *instance;
        struct arg_int *baud_rate;
        struct arg_int *data_bits;
        struct arg_str *parity;
        struct arg_int *stop_bits;
        struct arg_end *end;
    } uart_args;
#endif

#ifdef CONFIG_ESP_ADC_STREAM_ENABLED
    struct {
        struct arg_int *gpio;
        struct arg_int *output_sps;
        struct arg_int *averaging;
        struct arg_str *format;
        struct arg_end *end;
    } adc_args;
#endif
};

#ifdef CONFIG_ESP_DAP_MANAGE_WIFI
#define MAX_SSID_LEN            32
#define MAX_PASSWORD_LEN        64

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

static int wifi_cmd_handler(void *context, int argc, char **argv)
{
    struct command_context *ctx = context;
    FILE *out = ctx->out;
    int nerrors = arg_parse(argc, argv, (void **) &ctx->wifi_args);
    if (nerrors != 0) {
        arg_print_errors(out, ctx->wifi_args.end, argv[0]);
        fprintf(out, "Usage: wifi \"<ssid>\" \"<password>\" [auth_mode]\n");
        fprintf(out, "  auth_mode: open, wep, wpa, wpa2, wpa3 (default: wpa2)\n");
        return 1;
    }

    const char* ssid = ctx->wifi_args.ssid->sval[0];
    const char* password = ctx->wifi_args.password->sval[0];
    const char* auth_mode_str = ctx->wifi_args.auth_mode->count > 0 ?
        ctx->wifi_args.auth_mode->sval[0] : "wpa2";

    // Check for empty SSID. If empty, clear stored credentials.
    if (strlen(ssid) == 0) {
        fprintf(out, "Empty SSID provided. Clearing WiFi credentials from flash.\n");
        esp_err_t err = wifi_clear_config();
        if (err == ESP_OK) {
            fprintf(out, "WiFi credentials cleared successfully.\n");
            fprintf(out, "Reboot required to use hardcoded CONFIG values.\n");
        } else {
            fprintf(out, "Error clearing WiFi credentials: %s\n",
                    esp_err_to_name(err));
            return 1;
        }
        return 0;
    }

    // Validate input lengths.
    if (strlen(ssid) >= MAX_SSID_LEN) {
        fprintf(out, "Error: SSID too long (max %d characters)\n", MAX_SSID_LEN - 1);
        return 1;
    }
    if (strlen(password) >= MAX_PASSWORD_LEN) {
        fprintf(out, "Error: Password too long (max %d characters)\n",
                MAX_PASSWORD_LEN - 1);
        return 1;
    }

    wifi_auth_mode_t auth_mode = parse_auth_mode(auth_mode_str);

    fprintf(out, "WiFi credentials received:\n");
    fprintf(out, "  SSID: %s\n", ssid);
    fprintf(out, "  Password: %s\n", password);
    fprintf(out, "  Auth mode: %s\n", auth_mode_str);

    esp_err_t err = wifi_save_config(ssid, password, auth_mode);
    if (err == ESP_OK) {
        fprintf(out, "WiFi credentials saved successfully.\n");
        fprintf(out, "Reboot required to apply new settings.\n");
    } else {
        fprintf(out, "Error saving WiFi credentials: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}
#endif // CONFIG_ESP_DAP_MANAGE_WIFI

#if defined(CONFIG_ESP_UART_BRIDGE_1_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_2_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_3_ENABLED)

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

static int uart_cmd_handler(void *context, int argc, char **argv)
{
    struct command_context *ctx = context;
    FILE *out = ctx->out;
    int nerrors = arg_parse(argc, argv, (void **) &ctx->uart_args);
    if (nerrors != 0) {
        arg_print_errors(out, ctx->uart_args.end, argv[0]);
        fprintf(out, "Usage: uart <instance> <baud_rate> <data_bits> <parity> "
                "<stop_bits>\n");
        fprintf(out, "  instance: which UART bridge to configure (1, 2, or 3)\n");
        fprintf(out, "  data_bits: 7 or 8\n");
        fprintf(out, "  parity: n (none), e (even), o (odd) -- case-insensitive\n");
        fprintf(out, "  stop_bits: 1 or 2\n");
        fprintf(out, "  Use baud_rate 0 to clear stored settings and revert to "
                "CONFIG defaults.\n");
        return 1;
    }

    int instance = ctx->uart_args.instance->ival[0];
    int baud_rate = ctx->uart_args.baud_rate->ival[0];
    int data_bits = ctx->uart_args.data_bits->ival[0];
    const char* parity_str = ctx->uart_args.parity->sval[0];
    int stop_bits = ctx->uart_args.stop_bits->ival[0];

    int uart_num = uart_bridge_instance_to_uart_num(instance);
    if (uart_num < 0) {
        fprintf(out, "Error: no enabled UART bridge instance %d.\n", instance);
        return 1;
    }

    // A baud rate of 0 clears the stored settings.
    if (baud_rate == 0) {
        fprintf(out, "Clearing UART settings from flash.\n");
        esp_err_t err = uart_bridge_clear_config(uart_num);
        if (err == ESP_OK) {
            fprintf(out, "UART settings cleared successfully.\n");
            fprintf(out, "New connections will use CONFIG defaults.\n");
        } else {
            fprintf(out, "Error clearing UART settings: %s\n", esp_err_to_name(err));
            return 1;
        }
        return 0;
    }

    if (baud_rate < 0) {
        fprintf(out, "Error: baud rate must be positive.\n");
        return 1;
    }
    if (data_bits != 7 && data_bits != 8) {
        fprintf(out, "Error: data bits must be 7 or 8.\n");
        return 1;
    }
    uart_parity_t parity;
    if (!parse_uart_parity(parity_str, &parity)) {
        fprintf(out, "Error: parity must be n, e, or o.\n");
        return 1;
    }
    if (stop_bits != 1 && stop_bits != 2) {
        fprintf(out, "Error: stop bits must be 1 or 2.\n");
        return 1;
    }

    fprintf(out, "UART settings received:\n");
    fprintf(out, "  Instance: %d\n", instance);
    fprintf(out, "  UART number: %d\n", uart_num);
    fprintf(out, "  Baud rate: %d\n", baud_rate);
    fprintf(out, "  Data bits: %d\n", data_bits);
    fprintf(out, "  Parity: %s\n", parity_str);
    fprintf(out, "  Stop bits: %d\n", stop_bits);

    uart_word_length_t data_bits_enum =
            data_bits == 7 ? UART_DATA_7_BITS : UART_DATA_8_BITS;
    uart_stop_bits_t stop_bits_enum =
            stop_bits == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;

    esp_err_t err = uart_bridge_save_config(uart_num, baud_rate,
            data_bits_enum, parity, stop_bits_enum);
    if (err == ESP_OK) {
        fprintf(out, "UART settings saved successfully.\n");
    } else {
        fprintf(out, "Error saving UART settings: %s\n", esp_err_to_name(err));
        return 1;
    }

    err = uart_bridge_apply_live_config(uart_num, baud_rate, data_bits_enum,
            parity, stop_bits_enum);
    if (err == ESP_OK) {
        fprintf(out, "Applied immediately to UART%d.\n", uart_num);
    } else {
        fprintf(out, "Error applying settings immediately: %s\n",
                esp_err_to_name(err));
        fprintf(out, "New connections will still use the saved settings.\n");
    }
    return 0;
}
#endif

#ifdef CONFIG_ESP_ADC_STREAM_ENABLED
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

static int adc_cmd_handler(void *context, int argc, char **argv)
{
    struct command_context *ctx = context;
    FILE *out = ctx->out;
    int nerrors = arg_parse(argc, argv, (void **) &ctx->adc_args);
    if (nerrors != 0) {
        arg_print_errors(out, ctx->adc_args.end, argv[0]);
        fprintf(out, "Usage: adc <gpio> <output_sps> <averaging> <format>\n");
        fprintf(out, "  averaging: power of 2 from 1 to 1024 (1 = no averaging)\n");
        fprintf(out, "  format: text or binary16\n");
        fprintf(out, "  Use output_sps 0 to clear stored settings and revert to "
                "CONFIG defaults.\n");
        return 1;
    }

    int gpio = ctx->adc_args.gpio->ival[0];
    int output_sps = ctx->adc_args.output_sps->ival[0];
    int averaging = ctx->adc_args.averaging->ival[0];
    const char* format_str = ctx->adc_args.format->sval[0];

    // An output rate of 0 clears the stored settings. (gpio can't double as
    // the sentinel -- GPIO0 is a valid, already-default pin.)
    if (output_sps == 0) {
        fprintf(out, "Clearing ADC stream settings from flash.\n");
        esp_err_t err = adc_stream_clear_config();
        if (err == ESP_OK) {
            fprintf(out, "ADC stream settings cleared successfully.\n");
            fprintf(out, "New connections will use CONFIG defaults.\n");
        } else {
            fprintf(out, "Error clearing ADC stream settings: %s\n",
                    esp_err_to_name(err));
            return 1;
        }
        return 0;
    }

    if (output_sps < 0) {
        fprintf(out, "Error: output sample rate must be positive.\n");
        return 1;
    }
    if (averaging < 1 || averaging > 1024 ||
            (averaging & (averaging - 1)) != 0) {
        fprintf(out, "Error: averaging must be a power of 2 between 1 and 1024.\n");
        return 1;
    }
    // Same raw-rate sanity check enforced at connection time in
    // adc_stream_task() -- fail fast here instead of only at next connect.
    uint32_t raw_sample_freq_hz = (uint32_t)output_sps * (uint32_t)averaging;
    if (raw_sample_freq_hz < SOC_ADC_SAMPLE_FREQ_THRES_LOW ||
            raw_sample_freq_hz > SOC_ADC_SAMPLE_FREQ_THRES_HIGH) {
        fprintf(out, "Error: output rate (%d) x averaging (%d) = %" PRIu32
                " Hz raw ADC rate, outside the supported range (%d - %d "
                "Hz).\n", output_sps, averaging, raw_sample_freq_hz,
                SOC_ADC_SAMPLE_FREQ_THRES_LOW, SOC_ADC_SAMPLE_FREQ_THRES_HIGH);
        return 1;
    }
    enum adc_stream_format format;
    if (!parse_adc_format(format_str, &format)) {
        fprintf(out, "Error: format must be 'text' or 'binary16'.\n");
        return 1;
    }

    fprintf(out, "ADC stream settings received:\n");
    fprintf(out, "  GPIO: %d\n", gpio);
    fprintf(out, "  Output rate: %d Hz\n", output_sps);
    fprintf(out, "  Averaging: %d\n", averaging);
    fprintf(out, "  Format: %s\n", format_str);

    esp_err_t err = adc_stream_save_config(gpio, output_sps, averaging, format);
    if (err == ESP_OK) {
        fprintf(out, "ADC stream settings saved successfully.\n");
        fprintf(out, "New connections will use these settings.\n");
    } else {
        fprintf(out, "Error saving ADC stream settings: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}
#endif

static int reboot_cmd_handler(void *context, int argc, char **argv)
{
    struct command_context *ctx = context;
    fprintf(ctx->out, "Rebooting...\n");
    reboot();   // Does not return.
    return 0;
}

#if defined(CONFIG_ESP_DAP_MANAGE_WIFI) && defined(CONFIG_LWIP_IPV6)
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

static int status_cmd_handler(void *context, int argc, char **argv)
{
    struct command_context *ctx = context;
    FILE *out = ctx->out;

#ifdef CONFIG_ESP_DAP_MANAGE_WIFI
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    if (netif == NULL) {
        fprintf(out, "WiFi interface not found.\n");
        return 0;
    }

    bool connected;
    const char *ssid;
    int rssi;
    wifi_get_status(&connected, &ssid, &rssi);

    if (!connected) {
        fprintf(out, "Not connected to WiFi SSID: '%s'\n", ssid);
    }
    else {
        fprintf(out, "Connected to WiFi SSID: '%s'. RSSI: %d dBm\n", ssid,
                rssi);

        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
            fprintf(out, "Failed reading IP address.\n");
        }
        else {
            if (ip_info.ip.addr == 0) {
                fprintf(out, "No IP address assigned.\n");
            }
            else {
                fprintf(out, "IP address: " IPSTR "\n", IP2STR(&ip_info.ip));
            }
        }

#ifdef CONFIG_LWIP_IPV6
        esp_ip6_addr_t ip6_addrs[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
        int count = esp_netif_get_all_ip6(netif, ip6_addrs);

        if (count == 0) {
            fprintf(out, "No IPv6 address assigned.\n");
        }
        for(int i=0; i<count; i++) {
            esp_ip6_addr_type_t ipv6_type =
                esp_netif_ip6_get_addr_type(&ip6_addrs[i]);

            fprintf(out, "IPv6 address (%s): " IPV6STR "\n",
                    ipv6_type_str(ipv6_type), IPV62STR(ip6_addrs[i]));
        }
#endif
    }
#endif // CONFIG_ESP_DAP_MANAGE_WIFI

#ifdef CONFIG_ESP_DAP_SOCKET_CONSOLE_ENABLED
    fprintf(out, "Socket console: listening on port %d.\n",
            CONFIG_ESP_DAP_SOCKET_CONSOLE_TCP_PORT);
#endif

    cmsis_dap_print_status(out);

#if defined(CONFIG_ESP_UART_BRIDGE_1_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_2_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_3_ENABLED)
    uart_bridge_print_status(out);
#endif

#ifdef CONFIG_ESP_ADC_STREAM_ENABLED
    adc_stream_print_status(out);
#endif
    return 0;
}

static int help_cmd_handler(void *context, int argc, char **argv)
{
    struct command_context *ctx = context;
    FILE *out = ctx->out;
    fprintf(out, "Available commands:\n");
    fprintf(out, "  help - Show this help message.\n");
#ifdef CONFIG_ESP_DAP_MANAGE_WIFI
    fprintf(out, "  wifi \"<ssid>\" \"<password>\" [auth_mode] - Configure WiFi "
           "credentials.\n");
#endif
#if defined(CONFIG_ESP_UART_BRIDGE_1_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_2_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_3_ENABLED)
    fprintf(out, "  uart <instance> <baud_rate> <data_bits> <parity> <stop_bits> - "
           "Configure UART bridge settings.\n");
#endif
#ifdef CONFIG_ESP_ADC_STREAM_ENABLED
    fprintf(out, "  adc <gpio> <output_sps> <averaging> <format> - Configure "
           "ADC streaming settings.\n");
#endif
    fprintf(out, "  reboot - Restart the device.\n");
    fprintf(out, "  status - Report network status.\n");
    return 0;
}

static struct command_context *command_context_create(enum command_transport transport,
        FILE *out)
{
    struct command_context *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL)
        return NULL;
    ctx->out = out;
    ctx->transport = transport;

#ifdef CONFIG_ESP_DAP_MANAGE_WIFI
    ctx->wifi_args.ssid = arg_str1(NULL, NULL, "<ssid>", "WiFi network SSID");
    ctx->wifi_args.password =
        arg_str1(NULL, NULL, "<password>", "WiFi network password");
    ctx->wifi_args.auth_mode =
        arg_str0(NULL, NULL, "[auth_mode]", "Authentication mode: open, wep, "
                "wpa, wpa2, wpa3");
    ctx->wifi_args.end = arg_end(3);
#endif

#if defined(CONFIG_ESP_UART_BRIDGE_1_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_2_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_3_ENABLED)
    ctx->uart_args.instance = arg_int1(NULL, NULL, "<instance>",
            "Which UART bridge to configure (1, 2, or 3)");
    ctx->uart_args.baud_rate = arg_int1(NULL, NULL, "<baud_rate>",
            "UART baud rate (0 clears stored settings)");
    ctx->uart_args.data_bits = arg_int1(NULL, NULL, "<data_bits>",
            "Data bits: 7 or 8");
    ctx->uart_args.parity = arg_str1(NULL, NULL, "<parity>",
            "Parity: n, e, o (case-insensitive)");
    ctx->uart_args.stop_bits = arg_int1(NULL, NULL, "<stop_bits>",
            "Stop bits: 1 or 2");
    ctx->uart_args.end = arg_end(5);
#endif

#ifdef CONFIG_ESP_ADC_STREAM_ENABLED
    ctx->adc_args.gpio = arg_int1(NULL, NULL, "<gpio>", "ADC1 input GPIO");
    ctx->adc_args.output_sps = arg_int1(NULL, NULL, "<output_sps>",
            "Output sample rate in Hz (0 clears stored settings)");
    ctx->adc_args.averaging = arg_int1(NULL, NULL, "<averaging>",
            "Averaging count: power of 2, 1-1024");
    ctx->adc_args.format = arg_str1(NULL, NULL, "<format>",
            "Output format: text or binary16");
    ctx->adc_args.end = arg_end(4);
#endif

    return ctx;
}

#ifdef CONFIG_ESP_DAP_SOCKET_CONSOLE_ENABLED
#define MAX_CMD_LINE_LENGTH     256

// Dispatch table for process_socket_commands() (serial instead uses
// esp_console_cmd_register() in commands_init_serial()). void* matches
// esp_console_cmd_func_with_context_t, so handlers can be shared.
static const struct {
    const char *name;
    int (*func)(void *context, int argc, char **argv);
} socket_commands[] = {
#ifdef CONFIG_ESP_ADC_STREAM_ENABLED
    { "adc",    adc_cmd_handler },
#endif
    { "help",   help_cmd_handler },
    { "reboot", reboot_cmd_handler },
    { "status", status_cmd_handler },
#if defined(CONFIG_ESP_UART_BRIDGE_1_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_2_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_3_ENABLED)
    { "uart",   uart_cmd_handler },
#endif
#ifdef CONFIG_ESP_DAP_MANAGE_WIFI
    { "wifi",   wifi_cmd_handler },
#endif
};
#define NUM_COMMANDS (sizeof(socket_commands) / sizeof(socket_commands[0]))

static void command_context_destroy(struct command_context *ctx)
{
    if (ctx == NULL)
        return;
#ifdef CONFIG_ESP_DAP_MANAGE_WIFI
    arg_freetable((void **)&ctx->wifi_args,
            sizeof(ctx->wifi_args) / sizeof(void *));
#endif
#if defined(CONFIG_ESP_UART_BRIDGE_1_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_2_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_3_ENABLED)
    arg_freetable((void **)&ctx->uart_args,
            sizeof(ctx->uart_args) / sizeof(void *));
#endif
#ifdef CONFIG_ESP_ADC_STREAM_ENABLED
    arg_freetable((void **)&ctx->adc_args,
            sizeof(ctx->adc_args) / sizeof(void *));
#endif
    free(ctx);
}

static void commands_run_socket(struct command_context *ctx, char *line)
{
    char *argv[MAX_CMD_LINE_ARGS];
    size_t argc = esp_console_split_argv(line, argv, MAX_CMD_LINE_ARGS);
    if (argc == 0)
        return;     // empty (or all-whitespace) line

    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        if (strcmp(argv[0], socket_commands[i].name) == 0) {
            int ret = socket_commands[i].func(ctx, (int)argc, argv);
            if (ret != 0)
                fprintf(ctx->out, "Command returned non-zero error code: %d\n", ret);
            fflush(ctx->out);
            return;
        }
    }
    fprintf(ctx->out, "Unrecognized command\n");
    fflush(ctx->out);
}

static void commands_complete_socket(const char *buf, void *cb_ctx,
        void (*add)(void *cb_ctx, const char *str))
{
    size_t len = strlen(buf);
    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        if (strncmp(buf, socket_commands[i].name, len) == 0)
            add(cb_ctx, socket_commands[i].name);
    }
}

// esp_linenoise_get_line() can't distinguish a dead connection from an
// empty line -- both return an empty string. Check the socket directly.
static bool peer_closed(int fd)
{
    char buf;
    ssize_t n = recv(fd, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0)
        return true;                                // orderly close
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        return true;                                 // real error
    return false;
}

// Runs one socket connection's REPL to completion. f/fd remain owned by
// the caller throughout.
void process_socket_commands(FILE *f)
{
    int fd = fileno(f);
    struct command_context *ctx =
        command_context_create(COMMAND_TRANSPORT_SOCKET, f);
    if (ctx == NULL) {
        fprintf(stderr, "Command socket: out of memory creating context (fd %d).\n", fd);
        return;
    }

    esp_linenoise_config_t ln_config;
    esp_linenoise_get_instance_config_default(&ln_config);
    ln_config.prompt = COMMANDS_PROMPT;
    ln_config.max_cmd_line_length = MAX_CMD_LINE_LENGTH;
    ln_config.in_fd = fd;
    ln_config.out_fd = fd;
    ln_config.completion_cb = commands_complete_socket;
    ln_config.allow_dumb_mode = true;  // plain clients (e.g. netcat) don't speak ANSI

    esp_linenoise_handle_t handle;
    if (esp_linenoise_create_instance(&ln_config, &handle) != ESP_OK) {
        fprintf(stderr, "Command socket: failed to create linenoise instance (fd %d).\n",
                fd);
        command_context_destroy(ctx);
        return;
    }

    // Terminals that reply to the dumb-mode probe (ESC[5n) don't send that
    // reply on its own -- telnet/nc queue it and flush it together with the
    // first line the user types. Strip the resulting "[0n"/"[3n" prefix
    // (ESC already consumed as a control char) from just the first line.
    bool dumb_mode = false;
    esp_linenoise_is_dumb_mode(handle, &dumb_mode);
    bool first_line = dumb_mode;

    char line[MAX_CMD_LINE_LENGTH];
    while (1) {
        esp_linenoise_get_line(handle, line, sizeof(line));
        if (peer_closed(fd))
            break;
        if (first_line) {
            first_line = false;
            if (line[0] == '[' && isdigit((unsigned char)line[1]) && line[2] == 'n')
                memmove(line, line + 3, strlen(line + 3) + 1);
        }
        if (line[0] != '\0') {
            esp_linenoise_history_add(handle, line);
            commands_run_socket(ctx, line);
        }
    }

    esp_linenoise_delete_instance(handle);
    command_context_destroy(ctx);
}
#endif // CONFIG_ESP_DAP_SOCKET_CONSOLE_ENABLED

void commands_init_serial(void)
{
    // Initialize console.
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = COMMANDS_PROMPT;
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

    // Disable the REPL's keystroke hints callback -- none of our commands
    // set .hint, so it has nothing to show and just prints garbage on some
    // terminals.
    linenoiseSetHintsCallback(NULL);

    // One context for serial's entire lifetime -- unlike socket, there's
    // only ever one registration per command, so esp_console's shared
    // table is fine here.
    struct command_context *ctx =
        command_context_create(COMMAND_TRANSPORT_SERIAL, stdout);
    if (ctx == NULL) {
        fprintf(stderr, "Serial console: out of memory creating context.\n");
        return;
    }

    printf("Enabling console commands.\n");

    // Deregister the built-in help handler so ours gets a clean slot.
    esp_console_deregister_help_command();

    // Register commands.
    const esp_console_cmd_t help_cmd = {
        .command = "help",
        .help = "Show available commands",
        .func_w_context = help_cmd_handler,
        .context = ctx,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&help_cmd));

    const esp_console_cmd_t reboot_cmd = {
        .command = "reboot",
        .help = "Restart the device",
        .func_w_context = reboot_cmd_handler,
        .context = ctx,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&reboot_cmd));

    const esp_console_cmd_t status_cmd = {
        .command = "status",
        .help = "Show device status",
        .func_w_context = status_cmd_handler,
        .context = ctx,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&status_cmd));

#ifdef CONFIG_ESP_DAP_MANAGE_WIFI
    const esp_console_cmd_t wifi_cmd = {
        .command = "wifi",
        .help = "Configure WiFi credentials",
        .func_w_context = wifi_cmd_handler,
        .context = ctx,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_cmd));
#endif

#if defined(CONFIG_ESP_UART_BRIDGE_1_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_2_ENABLED) || \
    defined(CONFIG_ESP_UART_BRIDGE_3_ENABLED)
    const esp_console_cmd_t uart_cmd = {
        .command = "uart",
        .help = "Configure UART bridge settings",
        .func_w_context = uart_cmd_handler,
        .context = ctx,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&uart_cmd));
#endif

#ifdef CONFIG_ESP_ADC_STREAM_ENABLED
    const esp_console_cmd_t adc_cmd = {
        .command = "adc",
        .help = "Configure ADC streaming settings",
        .func_w_context = adc_cmd_handler,
        .context = ctx,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&adc_cmd));
#endif

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
