# TLS certificates for authentication

TLS can be enabled to authenticate clients and encrypt all TCP traffic. These
certs are only used when `CONFIG_ESP_TLS_ENABLED=y`. When enabled, all sockets
(DAP, UART bridge, socket console, ADC, etc) require the use of TLS.

These certs are comiled into the firmware (not committed to git):

- `servercert.pem` -- the ESP32's own leaf cert (EC P-256)
- `prvtkey.pem` -- the ESP32's private key, matching `servercert.pem`
- `cacert.pem` -- the CA used to verify client certs (mutual TLS)

Client certs live separately in `host/certs/`, one per authorized
person/workstation. Use `host/cert_info.sh` to inspect either directory. Run
`host/run_stunnel.sh` on the host to create a localhost proxy for the device.

## Workflow 1: this project generates the CA and the ESP32 cert
The default -- `idf.py build` runs `gen_esp32_certs.sh` for you the first
time `CONFIG_ESP_TLS_ENABLED=y` builds (see `main/CMakeLists.txt`), so no
manual step is needed just to build. 2 client certs ("User 1"/"User 2") are
created by default; make more the same way if needed:

```sh
./host/gen_client_cert.sh "User 3"
```

```sh
CLIENT_NAME="User 1" HOST=<esp32-ip-or-hostname> ./host/run_stunnel.sh
CLIENT_NAME="User 2" HOST=<esp32-ip-or-hostname> ./host/run_stunnel.sh
```

Idempotent -- safe to rerun/reflash without invalidating existing certs.
Issue more clients anytime; the CA doesn't change. Names don't matter here
(throwaway CA); prefer real names/IDs once it isn't.

Already have a CA? Drop `ca.key` and `cacert.pem` into `host/certs/` before
building -- `gen_esp32_certs.sh` reuses it instead of generating a new one.

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
`checkHost`/`checkIP`), so it's fine to share across every unit.  With no
per-unit serial known at build time, it's usually the only option. Don't use an
IP/hostname: it would go stale on a roaming device, and isn't checked anyway.
