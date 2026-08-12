#!/bin/sh
#
# Generates (or reuses) a CA, then generates an ESP32 server cert/key signed
# by it, EC P-256 (required by CONFIG_ESP_TLS_ECDSA_ONLY_CIPHERSUITES), for
# CONFIG_ESP_TLS_ENABLED. The default CA is a throwaway test CA -- NOT for
# production use. To sign against your own PKI instead, drop your own
# ca.key + cacert.pem into OUT_DIR before running this: it's reused as-is
# rather than replaced. See main/certs/README.md for the full set of
# supported workflows, including providing the ESP32 cert yourself too.
#
# Skips work already done: reuses an existing CA in OUT_DIR, and skips
# server cert generation if main/certs/servercert.pem already exists. Pass
# FORCE=1 to regenerate everything, including the CA -- this invalidates
# every client cert already issued against the old one.
#
# You may pass OUT_DIR and SERVER_CN as environment variables. SERVER_CN
# isn't checked against connection address by this project's stunnel config
# (verifyChain without checkHost/checkIP -- it only verifies the CA chain,
# not server identity), so it doesn't need to be an IP/hostname; prefer a
# stable identifier (device name/serial) over a network address, especially
# for a mobile device that may roam across networks.
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

if [ -n "${FORCE}" ] || [ ! -f ca.key ] || [ ! -f cacert.pem ]; then
    echo "Generating CA..."
    openssl ecparam -name prime256v1 -genkey -noout -out ca.key
    openssl req -x509 -new -key ca.key -sha256 -days 3650 \
        -subj "/CN=cmsis_dap_tcp_esp32 test CA" -out cacert.pem
else
    echo "Reusing existing CA in ${OUT_DIR}."
fi

if [ -z "${FORCE}" ] && [ -f "${MAIN_CERTS_DIR}/servercert.pem" ]; then
    echo "Server cert already exists in ${MAIN_CERTS_DIR} -- skipping."
    echo "Pass FORCE=1 to regenerate."
    exit 0
fi

echo "Generating server cert (CN=${SERVER_CN})..."
openssl ecparam -name prime256v1 -genkey -noout -out prvtkey.pem
openssl req -new -key prvtkey.pem -subj "/CN=${SERVER_CN}" -out server.csr
openssl x509 -req -in server.csr -CA cacert.pem -CAkey ca.key -CAcreateserial \
    -days 3650 -sha256 -out servercert.pem
rm -f server.csr

cp prvtkey.pem servercert.pem cacert.pem "${MAIN_CERTS_DIR}/"

echo
echo "Server cert/key + CA copied to ${MAIN_CERTS_DIR}/ (embedded at build time)."
echo "CA left in ${OUT_DIR}/ -- see host/gen_client_cert.sh to issue client certs."
