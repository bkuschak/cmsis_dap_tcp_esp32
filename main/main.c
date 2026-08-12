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
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include "sdkconfig.h"

#include "cpu_usage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "DAP.h"
#include "cmsis_dap_tcp.h"
#include "uart_bridge.h"
#include "commands.h"
#include "reboot.h"

#ifdef CONFIG_ESP_DAP_MANAGE_WIFI
#include "wifi.h"
#endif

#ifdef CONFIG_ESP_DAP_SOCKET_CONSOLE_ENABLED
#include "socket_console.h"
#endif

#ifdef CONFIG_ESP_ADC_STREAM_ENABLED
#include "adc_stream.h"
#include "soc/soc_caps.h"
#endif

#if defined(CONFIG_ESP_DAP_1_LED_RGB) || \
    defined(CONFIG_ESP_DAP_2_LED_RGB) || \
    defined(CONFIG_ESP_DAP_3_LED_RGB)
#include "ws2812_led.h"
#endif

static uint8_t mac_addr[6];
static char mac_addr_str[16];

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
#ifdef CONFIG_ESP_DAP_MANAGE_WIFI
    if(ret == pdPASS)
        wifi_mark_service_started();
#endif
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
#ifdef CONFIG_ESP_DAP_MANAGE_WIFI
    if(ret == pdPASS)
        wifi_mark_service_started();
#endif
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
#ifdef CONFIG_ESP_DAP_MANAGE_WIFI
    if(ret == pdPASS)
        wifi_mark_service_started();
#endif
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

#ifdef CONFIG_ESP_DAP_SOCKET_CONSOLE_ENABLED
static BaseType_t socket_console_task_start(void)
{
    static const struct socket_console_config config = {
        .port               = CONFIG_ESP_DAP_SOCKET_CONSOLE_TCP_PORT,
#ifdef CONFIG_ESP_DAP_SOCKET_CONSOLE_USE_KEEPALIVE
        .keepalive_timeout  = CONFIG_ESP_DAP_SOCKET_CONSOLE_KEEPALIVE_TIMEOUT,
#else
        .keepalive_timeout  = 0,
#endif
    };

    return socket_console_start(&config, "socket_console_task", NULL);
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
        fprintf(stderr, "Get flash size failed\n");
        return;
    }
    // esp_flash_get_size() returns the size baked into the app image header
    // by CONFIG_ESPTOOLPY_FLASHSIZE, clamped to that value even if the chip
    // is bigger. esp_flash_get_physical_size() probes the actual chip via
    // JEDEC ID instead.
    uint32_t flash_size_probed = 0;
    esp_flash_get_physical_size(NULL, &flash_size_probed);
    printf("%" PRIu32 "MB %s flash (phys), %" PRIu32 "MB (configured)\n",
           flash_size_probed / (uint32_t)(1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" :
           "external",
           flash_size / (uint32_t)(1024 * 1024));
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

#ifdef CONFIG_ESP_DAP_SERIAL_CONSOLE_ENABLED
    commands_init_serial();
#endif

#ifdef CONFIG_ESP_DAP_MANAGE_WIFI
    /* Initialize WiFi and connect to AP. If unable to connect after retries,
     * then force a reboot in an attempt to recover.
     */
    if (wifi_init() != 0) {
        fprintf(stderr, "Restarting due to WiFi connection failures.\n");
        reboot();
    }
#endif

    if(cmsis_dap_tcp_task_start() != pdPASS) {
        fprintf(stderr, "Failed to start CMSIS-DAP-TCP task.\n");
    }

#ifdef CONFIG_ESP_DAP_2_ENABLED
    if(cmsis_dap_tcp_2_task_start() != pdPASS) {
        fprintf(stderr, "Failed to start second CMSIS-DAP-TCP task.\n");
    }
#endif

#ifdef CONFIG_ESP_DAP_3_ENABLED
    if(cmsis_dap_tcp_3_task_start() != pdPASS) {
        fprintf(stderr, "Failed to start third CMSIS-DAP-TCP task.\n");
    }
#endif

#ifdef CONFIG_ESP_UART_BRIDGE_1_ENABLED
    if(uart_bridge_task_start() != pdPASS) {
        fprintf(stderr, "Failed to start UART bridge task.\n");
    }
#endif

#ifdef CONFIG_ESP_UART_BRIDGE_2_ENABLED
    if(uart_bridge_2_task_start() != pdPASS) {
        fprintf(stderr, "Failed to start second UART bridge task.\n");
    }
#endif

#ifdef CONFIG_ESP_UART_BRIDGE_3_ENABLED
    if(uart_bridge_3_task_start() != pdPASS) {
        fprintf(stderr, "Failed to start third UART bridge task.\n");
    }
#endif

#ifdef CONFIG_ESP_ADC_STREAM_ENABLED
    if(adc_stream_task_start() != pdPASS) {
        fprintf(stderr, "Failed to start ADC stream task.\n");
    }
#endif

#ifdef CONFIG_ESP_DAP_SOCKET_CONSOLE_ENABLED
    if(socket_console_task_start() != pdPASS) {
        fprintf(stderr, "Failed to start socket console task.\n");
    }
#endif

#ifdef CONFIG_ESP_PRINT_CPU_USAGE
    xTaskCreatePinnedToCore(cpu_usage_task, "cpu_usage", 4096, NULL,
            CPU_USAGE_TASK_PRIO, NULL, tskNO_AFFINITY);
#endif
}
