# TLS certs (compiled into the firmware)

Only used when `CONFIG_ESP_TLS_ENABLED=y`. Three files, embedded via
`EMBED_TXTFILES` in `main/CMakeLists.txt`:

- `servercert.pem` -- the ESP32's own leaf cert (EC P-256)
- `prvtkey.pem` -- the ESP32's private key, matching `servercert.pem`
- `cacert.pem` -- the CA used to verify client certs (mutual TLS)

Not committed (see `.gitignore`) -- generate a test set with
`host/gen_esp32_certs.sh` (run once per deployment; the CA is a fixed
trust root, so re-running invalidates every client cert already issued
against it), or replace with certs from your own CA for real deployment.
Issue one client cert per person/workstation with `host/gen_client_cert.sh
<name>`. See `host/cert_info.sh` to inspect whatever's currently here, and
`host/run_stunnel.sh` for the host-side TLS-terminating proxy this
project's deployment model expects.
