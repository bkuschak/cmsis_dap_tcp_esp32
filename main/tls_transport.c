/*
 * SPDX-FileCopyrightText: Brian Kuschak <bkuschak@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared transport layer used by every TCP-serving module (cmsis_dap_tcp,
 * uart_bridge, adc_stream, socket_console). Wraps a plain socket fd behind
 * a uniform read/write/close API so those modules don't need to know
 * whether TLS is in use.
 *
 * This is currently a pure passthrough to plain sockets -- TLS support is
 * added in a later change.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tls_transport.h"

// Bounds the fd->handle registry used by transport_linenoise_read/write().
// Generous vs. actual concurrency: up to 3 CMSIS-DAP + 3 UART bridge + 1 ADC
// instances, plus however many simultaneous socket console connections.
#define TRANSPORT_MAX_HANDLES   16

struct transport_s {
    int fd;
};

static struct {
    int fd;
    transport_handle_t t;
} registry[TRANSPORT_MAX_HANDLES];
static portMUX_TYPE registry_mux = portMUX_INITIALIZER_UNLOCKED;

static void registry_add(transport_handle_t t)
{
    portENTER_CRITICAL(&registry_mux);
    for (int i = 0; i < TRANSPORT_MAX_HANDLES; i++) {
        if (registry[i].t == NULL) {
            registry[i].fd = t->fd;
            registry[i].t = t;
            break;
        }
    }
    portEXIT_CRITICAL(&registry_mux);
}

static void registry_remove(transport_handle_t t)
{
    portENTER_CRITICAL(&registry_mux);
    for (int i = 0; i < TRANSPORT_MAX_HANDLES; i++) {
        if (registry[i].t == t) {
            registry[i].t = NULL;
            registry[i].fd = -1;
            break;
        }
    }
    portEXIT_CRITICAL(&registry_mux);
}

static transport_handle_t registry_find(int fd)
{
    transport_handle_t found = NULL;
    portENTER_CRITICAL(&registry_mux);
    for (int i = 0; i < TRANSPORT_MAX_HANDLES; i++) {
        if (registry[i].t != NULL && registry[i].fd == fd) {
            found = registry[i].t;
            break;
        }
    }
    portEXIT_CRITICAL(&registry_mux);
    return found;
}

esp_err_t tls_transport_global_init(void)
{
    return ESP_OK;
}

transport_handle_t transport_wrap(int fd)
{
    struct transport_s *t = calloc(1, sizeof(*t));
    if (t == NULL) {
        close(fd);
        return NULL;
    }
    t->fd = fd;
    registry_add(t);
    return t;
}

void transport_set_nonblocking(transport_handle_t t)
{
    int flags = fcntl(t->fd, F_GETFL, 0);
    if (flags < 0) flags = 0;
    fcntl(t->fd, F_SETFL, flags | O_NONBLOCK);
}

int transport_fd(transport_handle_t t)
{
    return t->fd;
}

ssize_t transport_read(transport_handle_t t, void *buf, size_t len)
{
    return recv(t->fd, buf, len, 0);
}

ssize_t transport_write(transport_handle_t t, const void *buf, size_t len)
{
    return send(t->fd, buf, len, 0);
}

int transport_write_all(transport_handle_t t, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = transport_write(t, p + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Non-blocking socket with a full send buffer (or, once TLS
                // is added, a WANT_WRITE mid-handshake): wait briefly and
                // retry rather than treating this as fatal. A genuinely
                // dead peer is eventually caught here too, since TCP
                // keepalives (configured by each module right after
                // accept()) will turn into a real send() error once probes
                // are exhausted.
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

bool transport_peer_closed(transport_handle_t t)
{
    char buf;
    ssize_t n = recv(t->fd, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0)
        return true;                                 // orderly close
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        return true;                                 // real error
    return false;
}

bool transport_has_pending(transport_handle_t t)
{
    (void)t;
    return false;   // no TLS layer yet to have buffered application data
}

void transport_close(transport_handle_t t)
{
    if (t == NULL)
        return;
    registry_remove(t);
    close(t->fd);
    free(t);
}

ssize_t transport_linenoise_read(int fd, void *buf, size_t count)
{
    transport_handle_t t = registry_find(fd);
    if (t == NULL)
        return read(fd, buf, count);   // shouldn't happen; fall back to plain read
    return transport_read(t, buf, count);
}

ssize_t transport_linenoise_write(int fd, const void *buf, size_t count)
{
    transport_handle_t t = registry_find(fd);
    if (t == NULL)
        return write(fd, buf, count);  // shouldn't happen; fall back to plain write
    return transport_write(t, buf, count);
}

static ssize_t cookie_read(void *cookie, char *buf, size_t n)
{
    return transport_read((transport_handle_t)cookie, buf, n);
}

static ssize_t cookie_write(void *cookie, const char *buf, size_t n)
{
    int ret = transport_write_all((transport_handle_t)cookie, buf, n);
    return ret == 0 ? (ssize_t)n : -1;
}

static int cookie_close(void *cookie)
{
    transport_close((transport_handle_t)cookie);
    return 0;
}

FILE *transport_fopen(transport_handle_t t)
{
    cookie_io_functions_t io = {
        .read = cookie_read,
        .write = cookie_write,
        .seek = NULL,
        .close = cookie_close,
    };
    return fopencookie(t, "r+", io);
}
