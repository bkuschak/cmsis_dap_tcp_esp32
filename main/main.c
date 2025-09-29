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

#include <stdio.h>
#include <string.h>
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
    vTaskDelay(1000);
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
        printf("  auth_mode: wep, wpa, wpa2, wpa3 (default: wpa2)\n");
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

static int reboot_cmd_handler(int argc, char **argv)
{
    printf("Rebooting...\n");
    reboot();   // Does not return.
    return 0;
}

static int help_cmd_handler(int argc, char **argv)
{
    printf("Available commands:\n");
    printf("  help - Show this help message.\n");
    printf("  wifi \"<ssid>\" \"<password>\" [auth_mode] - Configure WiFi "
           "credentials.\n");
    printf("  reboot - Restart the device.\n");
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

    wifi_args.ssid = arg_str1(NULL, NULL, "<ssid>", "WiFi network SSID");
    wifi_args.password =
        arg_str1(NULL, NULL, "<password>", "WiFi network password");
    wifi_args.auth_mode =
        arg_str0(NULL, NULL, "[auth_mode]", "Authentication mode: wep, wpa, "
                "wpa2, wpa3");
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
            int rssi;
            esp_wifi_sta_get_rssi(&rssi);
            printf("Connected to WiFi SSID: '%s'. RSSI: %d dBm\n", wifi_ssid,
                    rssi);
        }
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
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

void app_main(void)
{
    // Initialize the JTAG/SWD port pins.
    DAP_Setup();

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

#ifdef CONFIG_ESP_UART_BRIDGE_ENABLED
    xTaskCreate(uart_bridge_task, "uart_bridge_task", 4096, NULL, 5, NULL);
#endif

    xTaskCreate(cmsis_dap_tcp_task, "cmsis_dap_tcp_task", 4096, NULL, 5, NULL);
    cmsis_dap_tcp_initialized = true;

#ifdef CONFIG_ESP_PRINT_CPU_USAGE
    xTaskCreatePinnedToCore(cpu_usage_task, "cpu_usage", 4096, NULL,
            CPU_USAGE_TASK_PRIO, NULL, tskNO_AFFINITY);
#endif
}
