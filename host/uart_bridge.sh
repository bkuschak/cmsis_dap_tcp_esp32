#!/bin/sh
#
# Run this script on the host machine to create local serial port that is
# bridged over TCP/IP to the ESP32 UART. The ESP32 must be configured with:
#
#     CONFIG_ESP_UART_BRIDGE_ENABLED=y
#
# You may pass HOST, PORT, and FILE environment variables if you like, or
# directly edit the values below.
#
# Then use any serial terminal program to open the PTY named ${FILE}.
#
#     screen /tmp/tty_uart
#
# Requires 'socat'.
#

# Assign variables if they are not already set by the environment.
HOST=${HOST:="192.168.1.5"}
PORT=${PORT:=4442}
FILE=${FILE:="/tmp/tty_uart"}

echo "Connecting to UART bridge ${HOST}:${PORT} via '${FILE}'..."
socat TCP:${HOST}:${PORT} PTY,link=${FILE},raw,echo=0
