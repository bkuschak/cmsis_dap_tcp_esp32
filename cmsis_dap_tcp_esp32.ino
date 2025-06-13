/*
 * Arduino sketch supporting CMSIS-DAP over TCP/IP.
 * Refer to the dap folder for the CMSIS-DAP sources.
 *
 * Runs on the ESP32-C6 currently. To support another board / CPU,
 * dap/DAP_config.h will need to be modified.
 *
 * The CMSIS-DAP commands and responses are sent over a TCP socket
 * rather than USB. Use the OpenOCD cmsis_dap_tcp driver to connect.
 * OpenOCD must be configured with settings similar to these:
 *
 *     adapter driver cmsis-dap
 *     cmsis-dap backend tcp
 *     cmsis-dap tcp host 192.168.1.4
 *     cmsis-dap tcp dap_port 4441
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
 * Supports:
 * - JTAG or SWD mode.
 * - DAP over tcp/XXXX
 * - target console serial port over tcp/XXXX
 * - SWO trace over tcp/XXXX
 *
 * Prerequisite libraries must be installed first:
 *     https://github.com/contrem/arduino-timer
 */

#include <arduino-timer.h>
#include <driver/gpio.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_mac.h>
#include <HTTPClient.h>
#include <lwip/sockets.h>
#include <lwipopts.h>
#include <lwip/stats.h>
#include <soc/gpio_struct.h>
#include <WiFi.h>

#include "DAP_config.h"
#include "cmsis_dap_tcp.h"
#include "wifi_password.h"

#if !defined(WIFI_SSID) || !defined(WIFI_PASSWORD)
#error "WIFI_SSID and WIFI_PASSWORD must be defined! Add them to wifi_password.h"
#endif

// Define if we should wait for USB serial port to be opened at boot.
#undef WAIT_FOR_USB_SERIAL_PORT

// Hardware version, for reporting to OpenOCD.
//#define HW_VERSION_MAJOR    1
//#define HW_VERSION_MINOR    0

#if 0
// Define a supported board. See below.
#define BOARD_XIAO_ESP32C6

// GPIO pins used on the board.
// Change these if necessary to match your hardware.
#if defined(BOARD_XIAO_ESP32C6)
#define GPIO_SWCLK          GPIO_NUM_21   // D3
#define GPIO_SWDIO          GPIO_NUM_22   // D4
#define GPIO_SRESET         GPIO_NUM_16   // D6
#define LED_PIN             LED_BUILTIN

#elif defined(BOARD_ESP32_C6_MINI_1)
#define GPIO_SWCLK          GPIO_NUM_21
#define GPIO_SWDIO          GPIO_NUM_22
#define GPIO_SWDIO_UNUSED   GPIO_NUM_23   // future experimental use of MOSI/MISO.
#define GPIO_SRESET         GPIO_NUM_16
#define LED_PIN             GPIO_NUM_7

#else
#error "Board not defined!"
#endif
#endif

static Timer<2> timer;
static bool led_state;
static int wifi_drop_count;
static uint8_t mac_addr[6];
static char mac_addr_str[16];

// FIXME
extern "C" {
extern int cmsis_dap_tcp_process(void);
extern int cmsis_dap_tcp_init(int port_number);
}

static void print_wifi_status()
{
  Serial.print("SSID:        ");
  Serial.println(WiFi.SSID());
  Serial.print("RSSI:        ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
  Serial.print("IP Address:  ");
  Serial.println(WiFi.localIP());
}

static void connect_wifi()
{
  // Attempt to connect to Wifi network:
  Serial.print("Attempting to connect to SSID '");
  Serial.print(WIFI_SSID);
  Serial.println("'");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  WiFi.mode(WIFI_MODE_STA);

  // Disable power save to improve WiFi performance.
  // https://github.com/espressif/arduino-esp32/issues/1484
  esp_wifi_set_ps (WIFI_PS_NONE);

  Serial.println();
  Serial.println("Connected to WiFi:");
  print_wifi_status();
}

static bool check_wifi_callback(void* arg)
{
  (void)arg;

  if(WiFi.status() != WL_CONNECTED) {
    if(++wifi_drop_count >= 6) {
      // If we are disconnected for awhile, reboot the ESP32.
      Serial.println("WiFi disconnected. Restarting ESP32...");
      ESP.restart();    // does not return
    }
    else {
      // Otherwise, attempt a reconnect.
      Serial.println("Reconnecting to WiFi...");
      WiFi.disconnect();
      connect_wifi();
    }
  }
  return true;
}

#if 0
static bool led_off_callback(void* arg)
{
  (void)arg;
  set_led(false);
  return false; // false to stop
}

static inline void set_led(bool enable)
{
  if (enable)
    digitalWrite(LED_PIN, HIGH);
  else
    digitalWrite(LED_PIN, LOW);
}

static inline void blink_led()
{
  // Pulse the LED on for 25 msec.
  set_led(true);
  timer.in(25, led_off_callback);
}
#endif

/// ----------------------------------------------------------------------------

void setup(void)
{
  // Init SWD JTAG pins.
  PORT_SWD_SETUP();

  // Init LED (active low).
  //pinMode(LED_PIN, OUTPUT);
  //digitalWrite(LED_PIN, HIGH);

  // Initialize USB serial port for debugging.
  Serial.begin(115200);
#ifdef WAIT_FOR_USB_SERIAL_PORT
  while (!Serial);
#endif
  Serial.print("\nESP32 cmsis_dap_tcp booting (HW version ");
#if 0
  Serial.print(HW_VERSION_MAJOR);
  Serial.print(".");
  Serial.print(HW_VERSION_MINOR);
  Serial.print(", SW version 0x");
  Serial.print(REMOTE_SWD_SW_VERSION, HEX);
  Serial.println(") ...");
#endif

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
  Serial.print("MAC address: ");
  Serial.println(mac_addr_str);

  // Connect to WiFi and monitor it every 10 seconds.
  connect_wifi();
  timer.every(10000, check_wifi_callback);

#if 0
  // Initialize the SWD library.
  uint16_t hw_version = ((HW_VERSION_MAJOR & 0xFF) << 8) |
                         (HW_VERSION_MINOR & 0xFF);
  int ret = remote_swd_server_init(
              serial_number, hw_version, REMOTE_SWD_TCP_PORT,
              swdio_swclk_init, swdio_input, swdio_output,
              swdio_write, swdio_read, swclk_send_pulse, set_srst);
  if(ret < 0)
    fprintf(stderr, "Failed initializing remote_swd server.\n");
#endif
  // Initialize the TCP server.
  cmsis_dap_tcp_init(4441);
}

void loop(void)
{
  cmsis_dap_tcp_process();

  // Update the timer.
  timer.tick();
}
