# TLS certs (compiled into the firmware)

Only used when `CONFIG_ESP_TLS_ENABLED=y`. Three files, embedded via
`EMBED_TXTFILES` in `main/CMakeLists.txt`:

- `servercert.pem` -- the ESP32's own leaf cert (EC P-256)
- `prvtkey.pem` -- the ESP32's private key, matching `servercert.pem`
- `cacert.pem` -- the CA used to verify client certs (mutual TLS)

Not committed (see `.gitignore`). Client certs live separately in
`host/certs/`, one per authorized person/workstation -- see
`host/cert_info.sh` to inspect either directory, and `host/run_stunnel.sh`
for the host-side TLS-terminating proxy.

## Workflow 1: this project generates the CA and the ESP32 cert

```sh
./host/gen_esp32_certs.sh            # CA + this ESP32's server cert
./host/gen_client_cert.sh "User 1"   # client cert, signed by that CA
./host/gen_client_cert.sh "User 2"   # another, same CA
```

```sh
CLIENT_NAME="User 1" HOST=<esp32-ip-or-hostname> ./host/run_stunnel.sh
CLIENT_NAME="User 2" HOST=<esp32-ip-or-hostname> ./host/run_stunnel.sh
```

Idempotent -- safe to rerun/reflash without invalidating existing certs.
Issue more clients anytime; the CA doesn't change. Names don't matter here
(throwaway CA); prefer real names/IDs once it isn't.

Already have a CA? Drop `ca.key`/`cacert.pem` into `host/certs/` first --
`gen_esp32_certs.sh` reuses it instead of generating a new one.

## Workflow 2: your own PKI provides all certs

For when the CA's private key never leaves your own process (offline
root, HSM, CSR-submission workflow) -- nothing here signs anything.

```sh
cp /path/to/your/cacert.pem      main/certs/cacert.pem
cp /path/to/your/esp32-cert.pem  main/certs/servercert.pem
cp /path/to/your/esp32-key.pem   main/certs/prvtkey.pem
```

Build and flash as usual. `gen_client_cert.sh` still works for client
certs if you also drop your CA's key in `host/certs/ca.key`.

## `SERVER_CN`

Not validated by this project's `stunnel` config (chain-only check, no
`checkHost`/`checkIP`), so it's fine to share across every unit -- and
with no per-unit serial at build time, usually the only option. Don't use
an IP/hostname: it'd go stale on a roaming device, and wasn't checked
anyway.
