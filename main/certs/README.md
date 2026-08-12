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

## Workflow 1: quick start, this project's own CA, two clients

Fine for testing, or a small deployment where you're fine trusting this
project's scripts as your PKI. Run from the repo root:

```sh
./host/gen_esp32_certs.sh          # generates a CA + this ESP32's server cert
./host/gen_client_cert.sh alice    # issues alice's client cert, signed by that CA
./host/gen_client_cert.sh bob      # issues bob's, same CA
```

Each person then points `run_stunnel.sh` at their own cert:

```sh
CLIENT_NAME=alice HOST=<esp32-ip-or-hostname> ./host/run_stunnel.sh
CLIENT_NAME=bob   HOST=<esp32-ip-or-hostname> ./host/run_stunnel.sh
```

`gen_esp32_certs.sh` is idempotent -- safe to build/reflash repeatedly
without regenerating (and thereby invalidating) everything. Issue more
client certs the same way at any time; the CA doesn't change.

## Workflow 2: your own PKI provides the CA, this project generates the ESP32 cert

Use this if you already run a CA (or want a dedicated one for this fleet)
and are willing to hand its private key to `gen_esp32_certs.sh` so it can
sign the ESP32's cert (and `gen_client_cert.sh` can keep signing new client
certs without your involvement each time).

```sh
mkdir -p host/certs
cp /path/to/your/ca.key    host/certs/ca.key
cp /path/to/your/cacert.pem host/certs/cacert.pem

./host/gen_esp32_certs.sh          # detects your CA, reuses it, signs a new server cert
./host/gen_client_cert.sh alice    # signed by your CA too
```

## Workflow 3: your own PKI provides everything, including the ESP32 cert

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
still works (this is workflow 2's client-cert half, usable independently).

## A note on `SERVER_CN`

`gen_esp32_certs.sh` accepts a `SERVER_CN` environment variable for the
ESP32 server cert's Common Name. This project's `stunnel` config verifies
the CA chain but not server identity (`verifyChain` without `checkHost`/
`checkIP`), so `SERVER_CN` isn't validated against whatever address you
connect to -- it doesn't need to be, and for a mobile device that may
roam across networks, *shouldn't* be, an IP or hostname. Prefer a stable
identifier (e.g. a device name or serial number) that stays correct
regardless of what network the device is on.
