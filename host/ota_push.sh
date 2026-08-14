#!/bin/sh
#
# Pushes a firmware image to the ESP32's raw TCP OTA service (requires
# CONFIG_ESP_OTA_ENABLED). No custom protocol -- just streams the image
# and lets the connection close; esp_ota_end() validates it on the device
# side. If the push is successful, the device reboots the new firmware
# immediately.
#
# Requires 'ncat' and 'dd'.
#
# Usage:
#
# Plaintext:
#   HOST=192.168.1.42 ./ota_push.sh firmware.bin
#
# Using TLS (with CONFIG_ESP_TLS_ENABLED=y):
#   HOST=192.168.1.42 CERT_DIR=host/certs ./ota_push.sh firmware.bin
#
#   This script talks TLS directly, NOT through host/run_stunnel.sh, because
#   stunnel closes the whole connection the instant our side half-closes,
#   discarding the device's OK/FAIL reply.
#

set -e

FIRMWARE=$1
HOST=${HOST:="192.168.1.5"}
PORT=${PORT:="4460"}
CERT_DIR=${CERT_DIR:-}
CLIENT_NAME=${CLIENT_NAME:="User1"}

if [ -z "${FIRMWARE}" ]; then
    echo "Usage: $0 <firmware.bin>" >&2
    exit 1
fi
if [ ! -f "${FIRMWARE}" ]; then
    echo "Firmware file not found: ${FIRMWARE}" >&2
    exit 1
fi

SIZE=$(wc -c < "${FIRMWARE}" | tr -d ' ')
echo "Pushing $(basename "${FIRMWARE}") (${SIZE} bytes) to ${HOST}:${PORT}..."

# dd's progress prints to stderr, so it's visible but never enters the pipe.
# -w bounds the connect; -q bounds the wait for a reply after EOF (returns
# sooner once the device closes).
if [ -n "${CERT_DIR}" ]; then
    # No --ssl-verify: the server cert's CN is a device name, not a
    # hostname, so ncat's verification (which checks both) can't pass.
    RESPONSE=$(dd if="${FIRMWARE}" bs=1k status=progress | \
        ncat --ssl -w 10 -q 60 \
        --ssl-cert "${CERT_DIR}/clients/${CLIENT_NAME}.pem" \
        --ssl-key "${CERT_DIR}/clients/${CLIENT_NAME}.key" \
        "${HOST}" "${PORT}")
else
    RESPONSE=$(dd if="${FIRMWARE}" bs=1k status=progress | \
        ncat -w 10 -q 60 "${HOST}" "${PORT}")
fi

echo "${RESPONSE}"
case "${RESPONSE}" in
    OK*)
        echo "OTA update succeeded; device is rebooting."
        ;;
    *)
        echo "OTA update failed." >&2
        exit 1
        ;;
esac
