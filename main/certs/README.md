# TLS sockets

TLS can be enabled to authenticate clients and encrypt all TCP traffic.
Certificates are used only when `CONFIG_ESP_TLS_ENABLED=y`. When enabled, all
sockets (DAP, UART bridge, socket console, ADC, etc) require the use of TLS.

These certs are compiled into the firmware (not committed to git):

- `servercert.pem` -- the ESP32's own leaf cert (EC P-256)
- `prvtkey.pem` -- the ESP32's private key, matching `servercert.pem`
- `cacert.pem` -- the CA used to verify client certs (mutual TLS)

Client certs live separately in `host/certs/clients/`, one per authorized
person/workstation. The CA's private key (`ca.key`) lives in `host/certs/`
too, never in `main/certs/` -- the ESP32 only ever needs the CA's *public*
cert to verify clients, never the key that signs them, so keeping the key out
of the directory that gets embedded means it can't end up in a flash dump
even by accident. Use `host/cert_info.sh` to inspect either directory.

Note: currently we don't use unique server certificates for each device. Certs
are shared across every board preset you build, regardless of which workflow
below populates them. One CA cert and one set of client certs work across all
devices. Sharing the server cert/key this way should be safe: `stunnel` already
verifies the server cert chains to our CA, rejecting anyone else's. It doesn't
check *which* CA-signed device you're talking to (no `checkHost`/`checkIP`, see
`SERVER_CN` below), so `stunnel` wouldn't gain anything from a unique key per
board. This might be improved in the future if there's a need.

## Workflow 1: this project generates all the certificates
This is the default. `idf.py build` runs `gen_esp32_certs.sh` the first time
`CONFIG_ESP_TLS_ENABLED=y` builds so no manual step is needed. (See
`main/CMakeLists.txt`). Two client certs ("User1"/"User2") are created by
default. Make more the same way if needed:

```sh
./host/gen_client_cert.sh "User3"
```

Idempotent -- safe to rerun/reflash without invalidating existing certs.
Issue more clients anytime; the CA doesn't change. Names don't matter here.

Pass `FORCE=1` to `gen_esp32_certs.sh` to regenerate the CA and server cert
-- this deletes every existing client cert (they no longer verify against
the new CA) and reissues `User1`/`User2` fresh. Still produces one shared
cert, not distinct certs per board.

Already have a CA? Drop `ca.key` and `cacert.pem` into `host/certs/` before
building -- `gen_esp32_certs.sh` reuses it instead of generating a new one.

## Workflow 2: your PKI provides all the certificates

Use this method when the CA's private key never leaves your own process
(offline root, HSM, CSR-submission workflow). You must generate the CA and
esp32 certificates and place them into this directory:

```sh
cp /path/to/your/cacert.pem      main/certs/cacert.pem
cp /path/to/your/esp32-cert.pem  main/certs/servercert.pem
cp /path/to/your/esp32-key.pem   main/certs/prvtkey.pem
```

Place your client certs and keys into this directory:

```sh
cp /path/to/your/client1.pem     host/certs/clients/client1.pem
cp /path/to/your/client1.key     host/certs/clients/client1.key
```

Build and flash as usual.

## Note about `SERVER_CN`

`SERVER_CN` is not validated by this project's `stunnel` config (chain-only
check, no `checkHost`/`checkIP`), so it's fine to share across every unit.
With no per-unit serial known at build time, it's usually the only option.
Don't use an IP/hostname: it would go stale on a roaming device, and isn't
checked anyway.

## Usage

After loading the firmware onto your board, run `host/run_stunnel.sh` on the
host to create a localhost proxy for the device. This will redirect your
existing clients (OpenOCD, telnet, ncat, etc) over the TLS sockets. They don't
need to know anything about TLS.

```sh
CLIENT_NAME="User1" HOST=<esp32-ip-or-hostname> ./host/run_stunnel.sh
CLIENT_NAME="User2" HOST=<esp32-ip-or-hostname> ./host/run_stunnel.sh
```

Then connect to the proxy `localhost:<port>` instead of
`esp32-ip-or-hostname:<port>`. For CMSIS-DAP-TCP change your connect string to:

```sh
cmsis-dap tcp host localhost
cmsis-dap tcp port 4441
```

This way only authenticated clients can connect to your hardware and all
traffic is encrypted. There will be a modest performance impact (15% lower
throughput observed on SRAM writes using ESP32-S3).

