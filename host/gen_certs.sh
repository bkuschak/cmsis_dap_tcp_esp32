#!/bin/sh
#
# Generates a throwaway CA + ESP32 server cert/key + one client cert/key,
# all EC P-256 (required by CONFIG_ESP_TLS_ECDSA_ONLY_CIPHERSUITES), for
# testing CONFIG_ESP_TLS_ENABLED end to end. NOT for production use --
# replace with certs from your own CA before deploying for real.
#
# Server cert/key + CA are copied into main/certs/ (embedded into the
# firmware at build time). Client cert/key + a copy of the CA are left in
# OUT_DIR for host-side tools (stunnel, openssl s_client) to use.
#
# You may pass OUT_DIR and SERVER_CN (should match how you reach the
# ESP32, e.g. its IP) as environment variables.
#
# Requires 'openssl'.
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR=${OUT_DIR:="${SCRIPT_DIR}/certs"}
SERVER_CN=${SERVER_CN:="esp32-dap"}
MAIN_CERTS_DIR="${SCRIPT_DIR}/../main/certs"

set -e
mkdir -p "${OUT_DIR}" "${MAIN_CERTS_DIR}"
cd "${OUT_DIR}"

echo "Generating CA..."
openssl ecparam -name prime256v1 -genkey -noout -out ca.key
openssl req -x509 -new -key ca.key -sha256 -days 3650 \
    -subj "/CN=cmsis_dap_tcp_esp32 test CA" -out cacert.pem

echo "Generating server cert (CN=${SERVER_CN})..."
openssl ecparam -name prime256v1 -genkey -noout -out prvtkey.pem
openssl req -new -key prvtkey.pem -subj "/CN=${SERVER_CN}" -out server.csr
openssl x509 -req -in server.csr -CA cacert.pem -CAkey ca.key -CAcreateserial \
    -days 3650 -sha256 -out servercert.pem
rm -f server.csr

echo "Generating client cert..."
openssl ecparam -name prime256v1 -genkey -noout -out client.key
openssl req -new -key client.key -subj "/CN=test-client" -out client.csr
openssl x509 -req -in client.csr -CA cacert.pem -CAkey ca.key -CAcreateserial \
    -days 3650 -sha256 -out client.pem
rm -f client.csr

echo
echo "Client cert serial (for a future cert allowlist/blocklist):"
openssl x509 -in client.pem -noout -serial

cp prvtkey.pem servercert.pem cacert.pem "${MAIN_CERTS_DIR}/"

echo
echo "Server cert/key + CA copied to ${MAIN_CERTS_DIR}/ (embedded at build time)."
echo "Client cert/key + CA left in ${OUT_DIR}/ for stunnel/openssl s_client -- see host/run_stunnel.sh and host/cert_info.sh."
