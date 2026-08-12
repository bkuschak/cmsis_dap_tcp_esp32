/*
 * SPDX-FileCopyrightText: Brian Kuschak <bkuschak@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared transport layer used by every TCP-serving module (cmsis_dap_tcp,
 * uart_bridge, adc_stream, socket_console). Wraps a plain socket fd behind
 * a uniform read/write/close API so those modules don't need to know
 * whether TLS is in use.
 *
 * CONFIG_ESP_TLS_ENABLED is a single global build-time switch: when off,
 * this is a pure passthrough to plain sockets; when on, every connection
 * requires a mutually-authenticated TLS 1.2 handshake before any
 * application data is exchanged. There is no per-connection or runtime
 * choice between the two.
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

#if CONFIG_ESP_TLS_ENABLED
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#endif

// Bounds the fd->handle registry used by transport_linenoise_read/write().
// Generous vs. actual concurrency: up to 3 CMSIS-DAP + 3 UART bridge + 1 ADC
// instances, plus however many simultaneous socket console connections.
#define TRANSPORT_MAX_HANDLES   16

struct transport_s {
    int fd;
#if CONFIG_ESP_TLS_ENABLED
    mbedtls_net_context net;   // wraps fd for mbedtls_net_send/recv's bio ctx
    mbedtls_ssl_context ssl;
#endif
};

static struct {
    int fd;
    transport_handle_t t;
} registry[TRANSPORT_MAX_HANDLES];
static portMUX_TYPE registry_mux = portMUX_INITIALIZER_UNLOCKED;

// False means the registry is full (TRANSPORT_MAX_HANDLES too small for
// actual concurrency). The caller must not proceed with an unregistered
// handle: transport_linenoise_read/write() would fail to find it and fall
// back to raw fd I/O, silently bypassing TLS if enabled.
static bool registry_add(transport_handle_t t)
{
    bool added = false;
    portENTER_CRITICAL(&registry_mux);
    for (int i = 0; i < TRANSPORT_MAX_HANDLES; i++) {
        if (registry[i].t == NULL) {
            registry[i].fd = t->fd;
            registry[i].t = t;
            added = true;
            break;
        }
    }
    portEXIT_CRITICAL(&registry_mux);
    return added;
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

#if CONFIG_ESP_TLS_ENABLED

// Symbols for the PEM files embedded via EMBED_TXTFILES in
// main/CMakeLists.txt (main/certs/*.pem -- see that directory's README).
extern const uint8_t servercert_pem_start[] asm("_binary_servercert_pem_start");
extern const uint8_t servercert_pem_end[]   asm("_binary_servercert_pem_end");
extern const uint8_t prvtkey_pem_start[]    asm("_binary_prvtkey_pem_start");
extern const uint8_t prvtkey_pem_end[]      asm("_binary_prvtkey_pem_end");
extern const uint8_t cacert_pem_start[]     asm("_binary_cacert_pem_start");
extern const uint8_t cacert_pem_end[]       asm("_binary_cacert_pem_end");

static mbedtls_x509_crt server_cert;
static mbedtls_pk_context server_key;
static mbedtls_x509_crt ca_cert;
static mbedtls_ssl_config conf;

// ECDHE-ECDSA only (CONFIG_ESP_TLS_ECDSA_ONLY_CIPHERSUITES): much cheaper
// to handshake on this CPU than RSA. 0-terminated, per mbedtls's API.
#if CONFIG_ESP_TLS_ECDSA_ONLY_CIPHERSUITES
static const int ecdsa_ciphersuites[] = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    0,
};
#endif

// mbedtls_strerror() only understands its own composite error codes; a raw
// PSA status code (e.g. from an AEAD op) leaking through prints as a
// misleading match on unrelated low bits. Printing the decimal value too
// makes those recognizable (PSA_ERROR_* are small negative ints, see
// psa/crypto_values.h) without re-deriving it by hand.
static void log_mbedtls_error(const char *what, int ret)
{
    char buf[100];
    mbedtls_strerror(ret, buf, sizeof(buf));
    fprintf(stderr, "tls_transport: %s: %s (ret=%d)\n", what, buf, ret);
}

#endif // CONFIG_ESP_TLS_ENABLED

esp_err_t tls_transport_global_init(void)
{
#if CONFIG_ESP_TLS_ENABLED
    // mbedTLS 4.x / PSA Crypto: RNG is implicit once the PSA subsystem is
    // initialized -- no more manual mbedtls_ctr_drbg/entropy seeding or
    // mbedtls_ssl_conf_rng(), both removed from this mbedTLS version.
    psa_status_t psa_ret = psa_crypto_init();
    if (psa_ret != PSA_SUCCESS) {
        fprintf(stderr, "tls_transport: psa_crypto_init failed: %d\n", (int)psa_ret);
        return ESP_FAIL;
    }

    mbedtls_x509_crt_init(&server_cert);
    int ret = mbedtls_x509_crt_parse(&server_cert, servercert_pem_start,
            servercert_pem_end - servercert_pem_start);
    if (ret != 0) {
        log_mbedtls_error("failed to parse server cert", ret);
        return ESP_FAIL;
    }

    mbedtls_pk_init(&server_key);
    ret = mbedtls_pk_parse_key(&server_key, prvtkey_pem_start,
            prvtkey_pem_end - prvtkey_pem_start, NULL, 0);
    if (ret != 0) {
        log_mbedtls_error("failed to parse server private key", ret);
        return ESP_FAIL;
    }

    mbedtls_x509_crt_init(&ca_cert);
    ret = mbedtls_x509_crt_parse(&ca_cert, cacert_pem_start,
            cacert_pem_end - cacert_pem_start);
    if (ret != 0) {
        log_mbedtls_error("failed to parse CA cert", ret);
        return ESP_FAIL;
    }

    mbedtls_ssl_config_init(&conf);
    ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER,
            MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        log_mbedtls_error("ssl_config_defaults failed", ret);
        return ESP_FAIL;
    }

    // Require and verify a client cert -- this is what makes it mutual
    // TLS / access control, not just confidentiality.
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &ca_cert, NULL);
    mbedtls_ssl_conf_own_cert(&conf, &server_cert, &server_key);

    mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);

#if CONFIG_ESP_TLS_ECDSA_ONLY_CIPHERSUITES
    mbedtls_ssl_conf_ciphersuites(&conf, ecdsa_ciphersuites);
#endif

    printf("TLS: mutual TLS enabled; server cert/CA loaded.\n");
#endif // CONFIG_ESP_TLS_ENABLED
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

#if CONFIG_ESP_TLS_ENABLED
    t->net.fd = fd;
    mbedtls_ssl_init(&t->ssl);
    int ret = mbedtls_ssl_setup(&t->ssl, &conf);
    if (ret != 0) {
        log_mbedtls_error("ssl_setup failed", ret);
        mbedtls_ssl_free(&t->ssl);
        close(fd);
        free(t);
        return NULL;
    }
    mbedtls_ssl_set_bio(&t->ssl, &t->net, mbedtls_net_send, mbedtls_net_recv, NULL);

    // Bound the handshake -- most of this project's services accept only
    // one client at a time, so a client that opens the TCP connection but
    // never sends a ClientHello would otherwise wedge that service
    // indefinitely. Timeouts only apply for the handshake; cleared before
    // the connection enters its normal (non-blocking) data phase.
    struct timeval tv = {
        .tv_sec = CONFIG_ESP_TLS_HANDSHAKE_TIMEOUT_MS / 1000,
        .tv_usec = (CONFIG_ESP_TLS_HANDSHAKE_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    do {
        ret = mbedtls_ssl_handshake(&t->ssl);
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);

    struct timeval no_tv = {0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &no_tv, sizeof(no_tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &no_tv, sizeof(no_tv));

    if (ret != 0) {
        log_mbedtls_error("TLS handshake failed", ret);
        mbedtls_ssl_free(&t->ssl);
        close(fd);
        free(t);
        return NULL;
    }
    const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&t->ssl);
    if (peer != NULL) {
        char subject[128];
        mbedtls_x509_dn_gets(subject, sizeof(subject), &peer->subject);
        printf("TLS: client authenticated, cert subject: %s\n", subject);
    }
#endif // CONFIG_ESP_TLS_ENABLED

    if (!registry_add(t)) {
        fprintf(stderr, "tls_transport: handle registry full, dropping "
                "connection.\n");
        transport_close(t);
        return NULL;
    }
    return t;
}

void transport_set_nonblocking(transport_handle_t t)
{
    int flags = fcntl(t->fd, F_GETFL, 0);
    if (flags < 0) flags = 0;
    fcntl(t->fd, F_SETFL, flags | O_NONBLOCK);
}

void transport_set_keepalives(int fd, int keepalive_timeout)
{
    if (keepalive_timeout <= 0)
        return;

    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));
    val = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val));
    val = keepalive_timeout;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val));
}

int transport_fd(transport_handle_t t)
{
    return t->fd;
}

ssize_t transport_read(transport_handle_t t, void *buf, size_t len)
{
#if CONFIG_ESP_TLS_ENABLED
    int ret = mbedtls_ssl_read(&t->ssl, buf, len);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        return 0;
    if (ret < 0) {
        log_mbedtls_error("ssl_read failed", ret);
        errno = ECONNRESET;
        return -1;
    }
    return ret;
#else
    return recv(t->fd, buf, len, 0);
#endif
}

ssize_t transport_write(transport_handle_t t, const void *buf, size_t len)
{
#if CONFIG_ESP_TLS_ENABLED
    int ret = mbedtls_ssl_write(&t->ssl, buf, len);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    if (ret < 0) {
        log_mbedtls_error("ssl_write failed", ret);
        errno = ECONNRESET;
        return -1;
    }
    return ret;
#else
    return send(t->fd, buf, len, 0);
#endif
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
                // Non-blocking socket with a full send buffer, or mbedTLS
                // signaling WANT_READ/WANT_WRITE mid-record: wait briefly
                // and retry (with the same buf/len, satisfying mbedTLS's
                // retry contract) rather than treating this as fatal. A
                // genuinely dead peer is eventually caught here too, since
                // TCP keepalives (configured by each module right after
                // accept()) turn into a real error once probes are
                // exhausted.
                // pdMS_TO_TICKS(1) truncates to 0 at this project's 100Hz
                // tick rate, which would turn this into a busy taskYIELD()
                // spin instead of a real wait -- force at least 1 tick.
                TickType_t delay_ticks = pdMS_TO_TICKS(1);
                vTaskDelay(delay_ticks > 0 ? delay_ticks : 1);
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
#if CONFIG_ESP_TLS_ENABLED
    return mbedtls_ssl_check_pending(&t->ssl) != 0;
#else
    (void)t;
    return false;
#endif
}

void transport_close(transport_handle_t t)
{
    if (t == NULL)
        return;
    registry_remove(t);
#if CONFIG_ESP_TLS_ENABLED
    mbedtls_ssl_close_notify(&t->ssl);   // best-effort
    mbedtls_ssl_free(&t->ssl);
#endif
    close(t->fd);
    free(t);
}

ssize_t transport_linenoise_read(int fd, void *buf, size_t count)
{
    transport_handle_t t = registry_find(fd);
    if (t == NULL)
        return read(fd, buf, count);   // shouldn't happen; fall back to plain read
    ssize_t n = transport_read(t, buf, count);
    // esp_linenoise_dumb() only stops on a negative return, not 0/EOF --
    // translate so a closed connection doesn't spin it forever.
    if (n == 0) {
        errno = ECONNRESET;
        return -1;
    }
    return n;
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
