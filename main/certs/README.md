# TLS certs (compiled into the firmware)

Only used when `CONFIG_ESP_TLS_ENABLED=y`. Three files, embedded via
`EMBED_TXTFILES` in `main/CMakeLists.txt`:

- `servercert.pem` -- the ESP32's own leaf cert (EC P-256)
- `prvtkey.pem` -- the ESP32's private key, matching `servercert.pem`
- `cacert.pem` -- the CA used to verify client certs (mutual TLS)

Not committed (see `.gitignore`). Client certs (one per person/workstation
you authorize) live separately in `host/certs/`, not here -- see
`host/cert_info.sh` to inspect whatever's currently in either directory,
and `host/run_stunnel.sh` for the host-side TLS-terminating proxy this
project's deployment model expects.

## Workflow 1: this project generates the CA and the ESP32 cert

Fine for testing, or a small deployment where you're fine trusting this
project's scripts as your PKI. Run from the repo root:

```sh
./host/gen_esp32_certs.sh            # generates a CA + this ESP32's server cert
./host/gen_client_cert.sh "User 1"    # issues a client cert, signed by that CA
./host/gen_client_cert.sh "User 2"    # issues a second, same CA
```

Real names don't matter here -- any label works, since it's a throwaway
CA. Each person then points `run_stunnel.sh` at their own cert:

```sh
CLIENT_NAME="User 1" HOST=<esp32-ip-or-hostname> ./host/run_stunnel.sh
CLIENT_NAME="User 2" HOST=<esp32-ip-or-hostname> ./host/run_stunnel.sh
```

`gen_esp32_certs.sh` is idempotent -- safe to build/reflash repeatedly
without regenerating (and thereby invalidating) everything. Issue more
client certs the same way at any time; the CA doesn't change.

Already run your own CA and would rather use it than let this generate
one? Drop your `ca.key`/`cacert.pem` into `host/certs/` before running
`gen_esp32_certs.sh` -- same commands above, it detects and reuses your
CA instead. In that case this probably isn't a throwaway setup anymore,
so use real names/IDs for `gen_client_cert.sh` (quoted, so first + last
name works, e.g. `"Alice Smith"`) rather than generic placeholders.

## Workflow 2: your own PKI provides everything, including the ESP32 cert

Use this if the CA's private key never leaves your own process (e.g. an
offline root, an HSM, or a CSR-submission workflow) -- this project's
scripts aren't involved in signing anything.

```sh
cp /path/to/your/cacert.pem      main/certs/cacert.pem
cp /path/to/your/esp32-cert.pem  main/certs/servercert.pem
cp /path/to/your/esp32-key.pem   main/certs/prvtkey.pem
```

That's it -- build and flash as usual. For client certs, either issue them
through your own PKI process too, or, if you're willing to place your CA's
private key in `host/certs/ca.key` just for that purpose, `gen_client_cert.sh`
still works on its own.

## A note on `SERVER_CN`

`gen_esp32_certs.sh` accepts a `SERVER_CN` environment variable for the
ESP32 server cert's Common Name. This project's `stunnel` config verifies
the CA chain but not server identity (`verifyChain` without `checkHost`/
`checkIP`), so `SERVER_CN` isn't validated against whatever address you
connect to, and doesn't need to be unique per device -- it's fine (and,
since certs are baked in at build time with no per-unit serial available,
usually the only practical option) for every physical unit built from the
same firmware to share one generic, fleet-level name, similar in spirit to
`CONFIG_ESP_DAP_PRODUCT_ID`. It especially shouldn't be an IP or hostname,
since those go stale the moment a mobile device changes networks -- and
wouldn't have been checked against anything anyway.
