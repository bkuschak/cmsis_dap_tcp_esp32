#!/bin/sh
#
# Issues one client cert/key, EC P-256, signed by the CA created by
# gen_esp32_certs.sh. Run once per person/workstation you want to authorize
# for CONFIG_ESP_TLS_ENABLED -- the CA stays fixed, so this can be run as
# many times as needed without touching the ESP32's embedded certs.
#
# Usage: ./gen_client_cert.sh <name>
#   e.g. ./gen_client_cert.sh alice   ->  host/certs/alice.pem, alice.key
#
# You may pass OUT_DIR as an environment variable (must already contain
# ca.key/cacert.pem from gen_esp32_certs.sh).
#
# Requires 'openssl'.
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR=${OUT_DIR:="${SCRIPT_DIR}/certs"}
NAME=$1

if [ -z "${NAME}" ]; then
    echo "Usage: $0 <name>" >&2
    exit 1
fi
if [ ! -f "${OUT_DIR}/ca.key" ] || [ ! -f "${OUT_DIR}/cacert.pem" ]; then
    echo "No CA found in ${OUT_DIR} -- run gen_esp32_certs.sh first." >&2
    exit 1
fi

set -e
cd "${OUT_DIR}"

echo "Generating client cert (CN=${NAME})..."
openssl ecparam -name prime256v1 -genkey -noout -out "${NAME}.key"
openssl req -new -key "${NAME}.key" -subj "/CN=${NAME}" -out "${NAME}.csr"
openssl x509 -req -in "${NAME}.csr" -CA cacert.pem -CAkey ca.key -CAcreateserial \
    -days 3650 -sha256 -out "${NAME}.pem"
rm -f "${NAME}.csr"

echo
echo "Client cert serial (for a future cert allowlist/blocklist):"
openssl x509 -in "${NAME}.pem" -noout -serial

echo
echo "${OUT_DIR}/${NAME}.pem / ${NAME}.key -- pass CLIENT_NAME=${NAME} to"
echo "run_stunnel.sh / cert_info.sh to use this cert."
