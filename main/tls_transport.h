#ifndef TLS_TRANSPORT_H
#define TLS_TRANSPORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include "esp_err.h"

// Wraps either a plain socket fd or (once TLS support is added) an fd plus
// a TLS session, behind one read/write/close API -- so the TCP-serving
// modules (cmsis_dap_tcp, uart_bridge, adc_stream, socket_console) don't
// need to know or care which one they have.
typedef struct transport_s *transport_handle_t;

// One-time global setup. Call once, at boot.
esp_err_t tls_transport_global_init(void);

// Wrap a freshly accept()ed, still-blocking fd. Returns NULL on failure, in
// which case fd has already been closed by this function.
transport_handle_t transport_wrap(int fd);

void transport_set_nonblocking(transport_handle_t t);

// Raw fd, e.g. for FD_SET()/select().
int transport_fd(transport_handle_t t);

// recv()/send()-shaped: >0 bytes, 0 = orderly close, -1 with errno
// EAGAIN/EWOULDBLOCK on would-block, -1/other errno on real error.
ssize_t transport_read(transport_handle_t t, void *buf, size_t len);
ssize_t transport_write(transport_handle_t t, const void *buf, size_t len);

// Retries transport_write() until len bytes are sent or a real error
// occurs. 0 on success, -1 on error (errno set).
int transport_write_all(transport_handle_t t, const void *buf, size_t len);

// Non-blocking check for whether the peer has performed an orderly close,
// without consuming any data.
bool transport_peer_closed(transport_handle_t t);

// True if there is already-readable application data buffered above the
// raw socket layer that a select() on transport_fd() would not report.
bool transport_has_pending(transport_handle_t t);

void transport_close(transport_handle_t t);

// esp_linenoise_read_bytes_t/esp_linenoise_write_bytes_t-compatible
// callbacks, keyed on the raw fd (esp_linenoise's callback signature takes
// a bare int, not a pointer).
ssize_t transport_linenoise_read(int fd, void *buf, size_t count);
ssize_t transport_linenoise_write(int fd, const void *buf, size_t count);

// fopencookie()-backed FILE*, drop-in replacement for fdopen(fd, "r+").
FILE *transport_fopen(transport_handle_t t);

#ifdef __cplusplus
}
#endif

#endif
