#!/bin/sh
#
# Generates (or reuses) a CA, an ESP32 server cert/key signed by it, and two
# default client certs ("User1"/"User2", in OUT_DIR/clients/), all EC P-256
# (required by CONFIG_ESP_TLS_ECDSA_ONLY_CIPHERSUITES), for
# CONFIG_ESP_TLS_ENABLED. The default CA is a throwaway test CA -- NOT for
# production use. To sign against your own PKI instead, drop your own
# ca.key + cacert.pem into OUT_DIR before running this: it's reused as-is
# rather than replaced. If main/certs/servercert.pem is already provided
# externally and no local ca.key exists, this script skips CA/client-cert
# generation entirely rather than issuing certs that wouldn't verify. See
# main/certs/README.md for the full set of supported workflows.
#
# Skips work already done: reuses an existing CA, skips server cert
# generation if main/certs/servercert.pem already exists, and only creates
# "User1"/"User2" certs that don't already exist. Pass FORCE=1 to
# regenerate the CA and server cert -- this deletes every client cert
# already issued against the old CA (they'd no longer verify).
#
# You may pass OUT_DIR and SERVER_CN as environment variables. SERVER_CN
# isn't checked against connection address by this project's stunnel config
# (verifyChain without checkHost/checkIP -- it only verifies the CA chain,
# not server identity), so it doesn't need to be an IP/hostname, and doesn't
# need to be unique per device -- fine, and with no per-unit serial known at
# build time usually the only practical choice, for every unit built from
# the same firmware to share one generic fleet-level name.
#
# Requires 'openssl'.
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR=${OUT_DIR:="${SCRIPT_DIR}/certs"}
SERVER_CN=${SERVER_CN:="CMSIS-DAP-TCP device"}
MAIN_CERTS_DIR="${SCRIPT_DIR}/../main/certs"

set -e
mkdir -p "${OUT_DIR}" "${OUT_DIR}/clients" "${MAIN_CERTS_DIR}"
cd "${OUT_DIR}"

# A server cert already in main/certs/ with no local CA key means those certs
# came from an external PKI (Workflow 2) -- there's no way to sign client
# certs that would actually verify against that CA, so don't silently
# generate a throwaway CA/client certs that would never work. Not bypassed by
# FORCE: forcing here would overwrite the externally-provided server cert
# with one signed by a brand-new, unrelated CA.
if [ -f "${MAIN_CERTS_DIR}/servercert.pem" ] && [ ! -f ca.key ]; then
    echo "main/certs/servercert.pem already exists but no ca.key found in ${OUT_DIR} --"
    echo "assuming externally-provided certs (Workflow 2). Skipping CA/client-cert generation."
    echo "To auto-issue client certs against your CA, place ca.key + cacert.pem in ${OUT_DIR} and rerun."
    exit 0
fi

if [ -n "${FORCE}" ] || [ ! -f ca.key ] || [ ! -f cacert.pem ]; then
    echo "Generating CA..."
    if [ -n "${FORCE}" ]; then
        echo "FORCE set -- removing existing client certs (won't verify against the new CA)..."
        rm -rf "${OUT_DIR}/clients"
        mkdir -p "${OUT_DIR}/clients"
    fi
    openssl ecparam -name prime256v1 -genkey -noout -out ca.key
    openssl req -x509 -new -key ca.key -sha256 -days 3650 \
        -subj "/CN=cmsis_dap_tcp_esp32 CA" -out cacert.pem
else
    echo "Reusing existing CA in ${OUT_DIR}."
fi

if [ -n "${FORCE}" ] || [ ! -f "${MAIN_CERTS_DIR}/servercert.pem" ]; then
    echo "Generating server cert (CN=${SERVER_CN})..."
    openssl ecparam -name prime256v1 -genkey -noout -out prvtkey.pem
    openssl req -new -key prvtkey.pem -subj "/CN=${SERVER_CN}" -out server.csr
    openssl x509 -req -in server.csr -CA cacert.pem -CAkey ca.key -CAcreateserial \
        -days 3650 -sha256 -out servercert.pem
    rm -f server.csr
    # The server's own cert/key have no business sitting in the client cert
    # directory once copied; cacert.pem is symlinked (not copied) so
    # host/certs/ stays the single real copy, paired with ca.key.
    mv prvtkey.pem servercert.pem "${MAIN_CERTS_DIR}/"
    ln -sf "${OUT_DIR}/cacert.pem" "${MAIN_CERTS_DIR}/cacert.pem"
    echo "Server cert/key copied and CA symlinked to ${MAIN_CERTS_DIR}/ (embedded at build time)."
else
    echo "Server cert already exists in ${MAIN_CERTS_DIR} -- skipping."
fi

for name in "User1" "User2"; do
    if [ ! -f "${OUT_DIR}/clients/${name}.pem" ]; then
        "${SCRIPT_DIR}/gen_client_cert.sh" "${name}"
    fi
done

echo
echo "CA left in ${OUT_DIR}/ -- see host/gen_client_cert.sh to issue more client certs."
