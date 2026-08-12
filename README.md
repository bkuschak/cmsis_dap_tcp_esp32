# cmsis_dap_tcp for OpenOCD

OpenOCD supports the CMSIS-DAP protocol to communicate with a JTAG / SWD
programmer. Typically this is a local programmer with a USB connection. With
the addition of the OpenOCD cmsis_dap_tcp backend, the CMSIS-DAP protocol can
now run over TCP/IP instead of USB. This allows OpenOCD to connect to a remote
programmer over the network.

This project provides the remote-side implementation of the cmsis_dap_tcp
protocol, using an ESP32 as the remote programmer. It allows a cheap ESP32
board to program and debug an ARM microcontroller target. Both JTAG and the
two-wire SWD interface are supported. OpenOCD connects to the ESP32 using
TCP/IP over WiFi, allowing remote flashing and debugging of the target
board.

![diagram](img/cmsis_dap_tcp_diagram.svg)

- Tested with ESP32 S3, C3, and C6 boards as programmer and several STM32
  development boards (F4xx, F103, G0xx) as targets. A Lattice ECP5 FPGA target
  has also been used successfully.
- Either JTAG mode or SWD mode can be used to program the target. 2 GPIO are
  needed for SWD, or a minimum of 4 GPIO for JTAG.
- An optional GPIO pin can be used to drive the NRST# (SRST) signal, but this
  is typically not required.
- In JTAG mode, an optional GPIO pin can be used to drive the TRST signal, but
  this is typically not required.
- A separate GPIO can drive an activity LED controlled by OpenOCD (standard or
  RGB LED).
- UART to TCP/IP bridge can be enabled to provide access to the target board's
  serial console remotely, using an ESP32 UART.
- Up to 3 independent JTAG/SWD interfaces can be supported simultaneously.
- Up to 3 independent UART bridges can be supported simultaneously.
- Optional mutual TLS (mTLS) to authenticate clients and encrypt all TCP
  traffic.
- Typical performance:
  - SWD reading / writing SRAM: up to 200 KB/sec
  - SWD flashing a 512 KB firmware image to the STM32F401RE
  completes in about 13.4 seconds, including erase, program, and verify (with 4
  to 8 seconds of that time used for flash erasure). The Blue Pill takes about
  6 seconds for a 64KB image.
  - Performance depends on the quality of your WiFi network.

# Supported boards

The following boards were tested so far. They were chosen because they are
inexpensive and readily available from Amazon, AliExpress, Seeed Studio,
DigiKey, etc.

- Expressif [ESP32-S3 Devkit C1](https://www.digikey.com/en/products/detail/espressif-systems/ESP32-S3-DEVKITC-1-N8R8/15295894) and clones
- Unbranded HW-466AB [ESP32-C3 Super Mini](https://www.aliexpress.us/w/wholesale-esp32-c3-super-mini.html)
- Seeed Studio [XIAO ESP32-C6](https://www.seeedstudio.com/Seeed-Studio-XIAO-ESP32C6-p-5884.html)

Pinouts are configurable, but these are the defaults:

![ESP32-S3-Devkit-C1 pinout](img/esp32s3_devkitc_1.png)

![ESP32-C3 Super Mini pinout](img/esp32c3_super_mini.png)

![XIAO ESP32-C6 pinout](img/xiao_esp32c6_pinout.png)

The CMSIS-DAP code came from the Firmware directory of the [CMSIS-DAP
repo](https://github.com/ARM-software/CMSIS-DAP). ```DAP_config.h``` was then
modified to support the ESP32 GPIO.
```
commit 1fd47bed772ea40923472c90dfe11516e76033ee (HEAD -> main, tag: v2.1.2, origin/main, origin/HEAD)
```

# Limitations

The software has some limitations:

- SWO is currently unsupported.
- Maximum clock rate is ~6 MHz and duty cycle can vary from ~33% to ~66%.
- JTAG is significantly slower than SWD due to the way OpenOCD works.

# Building and Flashing the Firmware

This code requires the ESP-IDF build tools. Refer to the official
[installation guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html#installation)
and install them first. This project was tested using ESP-IDF v6.0.2.

Activate your ESP-IDF virtual environment:

```
. $HOME/esp/esp-idf/export.sh
```

The code supports different boards as 'presets'. They are described by the
CMakePresets.json file. Multiple board builds can coexist without interference
as each one uses its own build directory. To see the list of supported boards:

```
cmake --list-presets

Available configure presets:

  "board_esp32s3_devkit_c1"  - Espressif ESP32-S3 Devkit C1 board
  "board_yd_esp32_s3"        - VCC-GND Studio YD-ESP32-S3 (Devkit C1 clone)
  "board_esp32s3_zero"       - Waveshare ESP32-S3-Zero board
  "board_esp32c3_super_mini" - ESP32-C3 SuperMini board
  "board_xiao_esp32c6"       - Seeed Studio Xiao ESP32-C6 board
  "board_xiao_esp32c6_alt"   - Seeed Studio Xiao ESP32-C6 board (alternate pins for UART bridge)
```

If you are using one of the supported boards, type <b>one</b> of the following
lines, and all subsequent ```idf.py``` commands will target that specific
build.

```
export IDF_PRESET=board_esp32s3_devkit_c1
export IDF_PRESET=board_yd_esp32_s3
export IDF_PRESET=board_esp32s3_zero
export IDF_PRESET=board_esp32c3_super_mini
export IDF_PRESET=board_xiao_esp32c6
export IDF_PRESET=board_xiao_esp32c6_alt
```

Then proceed with the build and installation:

```
idf.py fullclean menuconfig build flash
```

* In menuconfig, goto to the "CMSIS-DAP configuration" page.

  <img src="img/menuconfig1.png" width="75%" />

* Hardcoded WiFi credentials can be configured on the "WiFi configuration"
  subpage.  (If you are not using WPA2, you might need to adjust the WiFi Scan
  auth mode threshold). WiFi credentials can also be changed at runtime using
  the ```wifi``` console command, and these will be stored in flash memory.

  <img src="img/menuconfig2.png" width="75%" />

* If needed, you can change the GPIO port pins for JTAG, SWD, reset, and LED on
  the "GPIO number assignments" subpage. The signals can be disabled if they
  are not needed. Up to 3 indepedent CMSIS-DAP interfaces can be enabled, if
  enough pins are available. Drive strength for the JTAG/SWD pins can be
  changed. The weakest drive is selected by default to minimize the effects of
  crosstalk and ringing when using cables.

  <img src="img/menuconfig3.png" width="75%" />

* If you want to use the UART to TCP/IP bridge, it can be configured on the
  the "UART to TCP/IP bridge" subpage. The baud rate and other default
  settings can be changed at runtime using the ```uart``` console command. A
  script ```host/uart_bridge.sh``` is provided that uses ```socat``` to present
  the remote UART as a pseudo-tty that can be opened using any serial terminal
  program on the host. The first UART bridge uses UART1 by default. Up to 3
  independent UART bridges can be enabled, if enough UARTs are available.

  <img src="img/menuconfig4.png" width="75%" />

* The console uses the native USB-Serial port. This port is non-blocking when
  no USB host is connected (with ESP-IDF v5.1+), so the device operates
  normally without a USB host attached.  UART0 is unused. If you want to use
  UART0 for console instead, update the console settings in menuconfig and
  assign the UART bridge to GPIO pins that do not conflict with the UART0
  pins.

   ```
   Component config → ESP System Settings → Channel for console output → Default UART
   Component config → ESP System Settings → Channel for console secondary output → No secondary
   CMSIS-DAP config → UART Bridge → Select GPIO numbers → enabled
   CMSIS-DAP config → UART Bridge → UART TX → (choose an available GPIO)
   CMSIS-DAP config → UART Bridge → UART RX → (choose an available GPIO)
   ```

* Optional mutual TLS (mTLS) authenticates clients and encrypts all TCP
  traffic. See [main/certs/README.md](main/certs/README.md) for more
  information.

   ```
   CMSIS-DAP config → Client authentication → Require certificates to connect (mutual TLS)
   ```

* It is also possible to stream voltage measurements from one ADC channel over
  another TCP/IP socket. Refer to the ADC streaming page. Sample rate,
  averaging, and other parameters are configurable in menuconfig and also using
  the ```adc``` console command.

   ```
   CMSIS-DAP config → ADC streaming → Enable ADC streaming
   ```

If you want to use this code as a component in another application see [this
section](#usage-as-a-component) below.

If you experience problems, additional debugging messages can be enabled in
menuconfig. This will impact performance.

```
CMSIS-DAP configuration → Enable debug logging for the CMSIS-DAP TCP server
```

# Running the Firmware

If you like, you can run the serial monitor to view and control the console. To
exit the serial monitor use ```Ctrl+]```.

```
idf.py monitor
```

Show the available console commands.

```
esp32> help
Available commands:
  help - Show this help message.
  wifi "<ssid>" "<password>" [auth_mode] - Configure WiFi credentials.
  uart <instance> <baud_rate> <data_bits> <parity> <stop_bits> - Configure UART bridge settings.
  reboot - Restart the device.
  status - Report network status.
esp32>
```

## Connecting to WiFi

After booting, the ESP32 will attempt to connect to WiFi. By default it will
use the hardcoded credentials provided in menuconfig. Optionally, you can
change these credentials at runtime using the command interface on the USB
serial console. To do this, use the ```wifi``` command and reboot afterwards:

```
esp32> wifi "my ssid" "my password" wpa2
esp32> reboot
```

To undo this and revert back to the hardcoded credentials, use empty strings:

```
esp32> wifi "" ""
esp32> reboot
```

If you must use an open WiFi without encryption, specify ```open``` with an
empty password:

```
esp32> wifi "guest wifi" "" open
esp32> reboot
```

After the ESP32 has connected to WiFi and obtained an IP address by DHCP you
can then run OpenOCD. The ESP32 will print status and error messages to the
console, including the WiFi connection status and IP address. A message is
printed whenever the OpenOCD client connects or disconnects. (Only one active
client is allowed).

You should see something like this from the ESP32:

```
CMSIS-DAP TCP running on ESP32
ESP-IDF version: v6.0.2
Hardware version: esp32c3 with 1 CPU core(s), WiFi/BLE, silicon revision v0.4, 2MB external flash
Minimum free heap size: 281772 bytes
MAC address: 70AF0912F000
Enabling console commands.
Attempting to connect to WiFi SSID: 'SomeWifiRouter'
Connected to WiFi SSID: 'SomeWifiRouter'. RSSI: -61 dBm
IP address: 192.168.0.198
Disabling WiFi power savings to improve performance.
cmsis_dap_tcp 1: JTAG/SWD, port 4441. GPIO (weakest): SWCLK=0 SWDIO=1 TDI=9 TDO=10 NTRST=7 NRESET=2 LED=8
cmsis_dap_tcp 2: SWD, port 4443. GPIO (weakest): SWCLK=3 SWDIO=4
cmsis_dap_tcp 3: SWD, port 4445. GPIO (weakest): SWCLK=6 SWDIO=5
UART bridge 1: using UART1 settings from flash: 115200-8-N-1
UART bridge 1: UART1, listening on port 4442. GPIOs: TX=21 RX=20
IPv6 address (link-local): fe80:0000:0000:0000:72af:09ff:fe12:f000
```

You can check the network status at any time by using the status command:

```
esp32>  status
Connected to WiFi SSID: 'SomeWifiRouter'. RSSI: -61 dBm
IP address: 192.168.0.198
IPv6 address (link-local): fe80:0000:0000:0000:72af:09ff:fe12:f000
cmsis_dap_tcp 1: JTAG/SWD, port 4441. GPIO (weakest): SWCLK=0 SWDIO=1 TDI=9 TDO=10 NTRST=7 NRESET=2 LED=8
cmsis_dap_tcp 2: SWD, port 4443. GPIO (weakest): SWCLK=3 SWDIO=4
cmsis_dap_tcp 3: SWD, port 4445. GPIO (weakest): SWCLK=6 SWDIO=5
UART bridge 1: UART1, listening on port 4442. 115200-8-N-1. GPIOs: TX=21 RX=20.
UART bridge 1: connected to client '192.168.0.55:63381'. Bytes: TX=2759 RX=32454.
```

# Building and Running OpenOCD

Get the latest source code from git. Configure and build it as usual:

```
git clone git://git.code.sf.net/p/openocd/code openocd
cd openocd
./bootstrap
./configure
make
```

An OpenOCD configuration file has been provided for convenience.
Edit your ```tcl/interface/cmsis-dap-tcp.cfg``` configuration file to point to
your ESP32's IP address:

```
adapter driver cmsis-dap
cmsis-dap backend tcp
cmsis-dap tcp host 192.168.1.107
cmsis-dap tcp port 4441
transport select swd
adapter speed 6000
reset_config none
```

If you are on a slow network, you might need to add this line to avoid short
timeouts that can lead to command mismatch errors in some cases. If so, specify
a longer timeout in milliseconds:

```
cmsis-dap tcp min_timeout 300
```

To flash an STM32 target, for example, run the following command from your
OpenOCD build directory.  Replace ```firmware.elf``` with the name of your
ELF file, and ```stm32f1x.cfg``` with the appropriate file for your
microcontroller.

```
./src/openocd --search tcl \
              -f tcl/interface/cmsis-dap-tcp.cfg \
              -f tcl/target/stm32f1x.cfg \
              -c "program firmware.elf verify reset exit"
```

To read/write SRAM, for example:

```
dd if=/dev/random of=random_96kb.bin bs=1024 count=96

./src/openocd --search tcl \
              -f tcl/interface/cmsis-dap-tcp.cfg \
              -f tcl/target/stm32f1x.cfg \
              -c "adapter speed 6000" \
              -c "init; halt; reset; poll off" \
              -c "load_image random_96kb.bin 0x20000000" \
              -c "dump_image /dev/null 0x20000000 0x18000" \
              -c "shutdown"
```

The LED normally illuminates whenever OpenOCD is connected to the ESP32. If you
want to control it manually or from scripts, you can use this OpenOCD command
to turn it off / on:

```
# cmsis-dap cmd <Command_ID 0x01> <LED_Selection> <LED_State>
cmsis-dap cmd 0x01 0x00 0x00
cmsis-dap cmd 0x01 0x00 0x01
```

Once everything is working you may disconnect the ESP32 from your PC and run
it as a standalone device. It can be powered by a USB charger. This could be
your normal use case, where the ESP32 is directly connected to a remote target,
and all debugging and flash programming is done over the network.

# Performance

The observed performance is summarized below. Your performance will depend on
your particular WiFi network environment. The C6 seems to suffer due to a
complex internal bus architecture that leads to lower throughput.

| Board | Chip / arch | CPU clock | SWCLK speed (max) | SRAM write (avg) | SRAM read (avg) |
|---|---|---|---|---|---|
| ESP32-S3 DevKitC-1 | ESP32-S3, Xtensa | 240 MHz | 5.3 MHz | ~200 KB/sec | ~80 KB/sec |
| ESP32-C3 Super Mini | ESP32-C3, RISC-V | 160 MHz | 5.0 MHz | ~175 KB/sec | ~80 KB/sec |
| Xiao ESP32-C6 (alt) | ESP32-C6, RISC-V | 160 MHz | 1.4 MHz | ~90 KB/sec | ~50 KB/sec |

<br>

![performance](img/performance.svg)

On the ESP32-S3 @ 240MHz, a single SWD 32-bit transfer completes in less than
10 microseconds, with a maximum SWCLK clock rate of ~5 MHz. Due to
bit-banging and clock domain crossing, the SWCLK duty cycle is not 50% and it
may vary slightly from one transfer to the next. An SWD read cycle is pictured
below. Yellow is SWCLK. Green is SWDIO.

<br>

![scopeshot1](img/scopeshot1.png)

<br>

![scopeshot1](img/scopeshot2.png)

Actual performance will depend on your WiFi network. For slow networks,
you might need to increase the ```cmsis-dap tcp min_timeout``` parameter if
you see error messages related to command mismatch.

Starting the OpenOCD server like this:

```
./src/openocd \
    --search tcl \
    -c "debug_level 2" \
    -c "adapter driver cmsis-dap" \
    -c "transport select swd" \
    -c "cmsis-dap backend tcp" \
    -c "cmsis-dap tcp host 192.168.1.107" \
    -c "cmsis-dap tcp port 4441" \
    -c "cmsis-dap tcp min_timeout 150" \
    -f "tcl/target/stm32f4x.cfg" \
    -c "reset_config none"

Open On-Chip Debugger 0.12.0+dev-01114-gbf01f1089 (2025-08-07-11:52)
Licensed under GNU GPL v2
For bug reports, read
	http://openocd.org/doc/doxygen/bugs.html
Info : CMSIS-DAP: using minimum timeout of 100 ms for TCP packets.
none separate
Info : Listening on port 6666 for tcl connections
Info : Listening on port 4444 for telnet connections
Info : CMSIS-DAP: Connecting to 192.168.1.107:4441 using TCP backend
Info : CMSIS-DAP: SWD supported
Info : CMSIS-DAP: JTAG supported
Info : CMSIS-DAP: Atomic commands supported
Info : CMSIS-DAP: Test domain timer supported
Info : CMSIS-DAP: FW Version = 2.1.2
Info : CMSIS-DAP: Serial# = E4B323B60EB4
Info : CMSIS-DAP: Interface Initialised (SWD)
Info : SWCLK/TCK = 0 SWDIO/TMS = 0 TDI = 0 TDO = 0 nTRST = 0 nRESET = 1
Info : CMSIS-DAP: Interface ready
Info : clock speed 2000 kHz
Info : SWD DPIDR 0x2ba01477
Info : [stm32f4x.cpu] Cortex-M4 r0p1 processor detected
Info : [stm32f4x.cpu] target has 6 breakpoints, 4 watchpoints
Info : [stm32f4x.cpu] Examination succeed
Info : [stm32f4x.cpu] starting gdb server on 3333
Info : Listening on port 3333 for gdb connections
Info : accepting 'telnet' connection on tcp/4444
```

## Using ESP32-S3 @ 240 MHz

Performance is highest on ESP32-S3. The throughput seems more variable
on each run, but here are some representative numbers connecting to an
STM32F401RE target and reading and writing SRAM:

```
% telnet localhost 4444
> poll off

> load_image ./random_96kb.bin 0x20000000
98304 bytes written at address 0x20000000
downloaded 98304 bytes in 0.489488s (196.123 KiB/s)

> dump_image /dev/null 0x20000000 0x18000
dumped 98304 bytes in 0.832846s (115.267 KiB/s)
```

## Using ESP32-C3 @ 160 MHz

ESP32-C3 running at 160 MHz is single core and lower frequency but its
performance in this case is just slightly below the S3.

## Using ESP32-C6 @ 160 MHz

Xiao ESP32C6 running at 160 MHz is considerably slower. After multiple
unsuccessful attempts to optimize peformance, it seems that the more complex
internal bus structure leads to lower performance for the GPIO bit-banging.

Connecting to an STM32F401RE target and reading and writing SRAM:

```
% telnet localhost 4444
> poll off

> load_image ./random_96kb.bin 0x20000000
98304 bytes written at address 0x20000000
downloaded 98304 bytes in 1.092678s (87.858 KiB/s)

> dump_image /dev/null 0x20000000 0x18000
dumped 98304 bytes in 1.469766s (65.317 KiB/s)
```

Programming and verifying a 512 KB flash image takes about 20 seconds:

```
time ./src/openocd \
    --search tcl \
    -c "debug_level 2" \
    -c "adapter driver cmsis-dap" \
    -c "transport select swd" \
    -c "cmsis-dap backend tcp" \
    -c "cmsis-dap tcp host 192.168.1.107" \
    -c "cmsis-dap tcp port 4441" \
    -f "tcl/target/stm32f4x.cfg" \
    -c "reset_config none" \
    -c "program ${ELF} verify reset exit"

Open On-Chip Debugger 0.12.0+dev-01114-gbf01f1089 (2025-08-07-11:52)
Licensed under GNU GPL v2
For bug reports, read
	http://openocd.org/doc/doxygen/bugs.html
none separate
Info : CMSIS-DAP: Connecting to 192.168.1.107:4441 using TCP backend
Info : CMSIS-DAP: SWD supported
Info : CMSIS-DAP: JTAG supported
Info : CMSIS-DAP: Atomic commands supported
Info : CMSIS-DAP: Test domain timer supported
Info : CMSIS-DAP: FW Version = 2.1.2
Info : CMSIS-DAP: Serial# = E4B323B60EB4
Info : CMSIS-DAP: Interface Initialised (SWD)
Info : SWCLK/TCK = 0 SWDIO/TMS = 0 TDI = 0 TDO = 0 nTRST = 0 nRESET = 1
Info : CMSIS-DAP: Interface ready
Info : clock speed 2000 kHz
Info : SWD DPIDR 0x2ba01477
Info : [stm32f4x.cpu] Cortex-M4 r0p1 processor detected
Info : [stm32f4x.cpu] target has 6 breakpoints, 4 watchpoints
Info : [stm32f4x.cpu] Examination succeed
Info : [stm32f4x.cpu] starting gdb server on 3333
Info : Listening on port 3333 for gdb connections
[stm32f4x.cpu] halted due to debug-request, current mode: Thread
xPSR: 0x01000000 pc: 0x08000734 msp: 0x20018000
** Programming Started **
Info : device id = 0x10016433
Info : flash size = 512 KiB
** Programming Finished **
** Verify Started **
** Verified OK **
** Resetting Target **
shutdown command invoked

real    0m19.242s
user    0m0.052s
sys     0m0.155s
```

## ADC streaming

As a development/debugging aid, live ADC measurements can be streamed and
plotted using the ```host/adc_stream_plot.py``` script.  Refer to the
experimental feature ```CONFIG_ESP_ADC_STREAM_ENABLED```. The ESP32 ADCs tend
to be noisy, so you might want to enable maximum averaging. Here is a plot of
the ADC reading from a current sense amplifier:

![scopeshot1](img/adc_streaming.png)

# Usage as a component

Two additional features were added by [@w531t4](https://github.com/w531t4) and
integrated into the project. Thank you!

This cmsis_dap_tcp server may be incorporated as a component in another
application.  Simply define your CMAKE_PROJECT_NAME as something other than
“cmsis_dap_tcp_esp32”.  This will cause ```main.c``` to be left out of the
project.  Replace the functionality of ```main.c``` with your own
implementation.  Be sure to call ```cmsis_dap_tcp_start(&config,
"cmsis_dap_tcp_task", …)``` and pass a valid ```cmsis_dap_tcp_config```
structure to define each interface.

Also, an ESPHome wrapper is available
[here](https://github.com/w531t4/ESPHome-cmsis_dap_tcp_esp32-Wrapper).
