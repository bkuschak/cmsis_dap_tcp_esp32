#!/bin/sh
#
# Runs stunnel on the host machine as a TLS-terminating proxy: local
# plaintext ports (matching this project's normal port numbers) forward to
# the ESP32's TLS ports over mutual TLS. Point OpenOCD, a serial terminal,
# etc. at 127.0.0.1:<port>, same as if CONFIG_ESP_TLS_ENABLED were off.
#
# The ESP32 must be running with CONFIG_ESP_TLS_ENABLED=y and the same CA
# as the client cert below (see gen_esp32_certs.sh / gen_client_cert.sh).
#
# You may pass HOST, CERT_DIR, and CLIENT_NAME (which client cert to
# present, if you've issued more than one via gen_client_cert.sh) as
# environment variables.
#
# Usage:
#   HOST=192.168.1.5 CLIENT_NAME="User1"  host/run_stunnel.sh
#
# Requires 'stunnel' (e.g. 'brew install stunnel').
#

HOST=${HOST:="192.168.1.5"}
CERT_DIR=${CERT_DIR:="$(dirname "$0")/certs"}
CLIENT_NAME=${CLIENT_NAME:="User1"}
CONF=$(mktemp)
trap 'rm -f "${CONF}"' EXIT

cat > "${CONF}" <<EOF
foreground = yes
client = yes
cert = ${CERT_DIR}/clients/${CLIENT_NAME}.pem
key = ${CERT_DIR}/clients/${CLIENT_NAME}.key
CAfile = ${CERT_DIR}/cacert.pem
verifyChain = yes
# notice: connection accept/reset events, without the verifyChain boilerplate
# or per-service TLS setup detail that "info" and above add.
debug = notice

[console]
accept = 127.0.0.1:4440
connect = ${HOST}:4440

[dap1]
accept = 127.0.0.1:4441
connect = ${HOST}:4441

[uart1]
accept = 127.0.0.1:4442
connect = ${HOST}:4442

[dap2]
accept = 127.0.0.1:4443
connect = ${HOST}:4443

[uart2]
accept = 127.0.0.1:4444
connect = ${HOST}:4444

[dap3]
accept = 127.0.0.1:4445
connect = ${HOST}:4445

[uart3]
accept = 127.0.0.1:4446
connect = ${HOST}:4446

[adc]
accept = 127.0.0.1:4451
connect = ${HOST}:4451
EOF

# No [ota] section: stunnel closes the whole connection as soon as the local
# side half-closes, discarding the device's OK/FAIL reply. ota_push.sh talks
# TLS directly (ncat --ssl) instead.

echo "Proxying local ports to ${HOST} via mutual TLS (Ctrl-C to stop)."
echo "Services not actually enabled on the ESP32 will just fail to connect -- harmless."
stunnel "${CONF}"
