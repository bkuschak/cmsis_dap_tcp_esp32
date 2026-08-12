# TLS certs (compiled into the firmware)

Only used when `CONFIG_ESP_TLS_ENABLED=y`. Three files, embedded via
`EMBED_TXTFILES` in `main/CMakeLists.txt`:

- `servercert.pem` -- the ESP32's own leaf cert (EC P-256)
- `prvtkey.pem` -- the ESP32's private key, matching `servercert.pem`
- `cacert.pem` -- the CA used to verify client certs (mutual TLS)

Not committed (see `.gitignore`) -- generate a test set with
`host/gen_certs.sh`, or replace with certs from your own CA for real
deployment. See `host/cert_info.sh` to inspect whatever's currently here,
and `host/run_stunnel.sh` for the host-side TLS-terminating proxy this
project's deployment model expects.
