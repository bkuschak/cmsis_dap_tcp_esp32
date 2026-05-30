/*
 * SPDX-FileCopyrightText: Brian Kuschak <bkuschak@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 *
 * TCP server for the CMSIS-DAP TCP protocol as used by OpenOCD.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "DAP.h"
#include "cmsis_dap_tcp.h"

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define le_to_h_u16(a)  (a)
#define le_to_h_u32(a)  (a)
#define h_u16_to_le(a)  (a)
#define h_u32_to_le(a)  (a)
#else
#include <byteswap.h>
#define le_to_h_u16(a)  __bswap_16(a)
#define le_to_h_u32(a)  __bswap_32(a)
#define h_u16_to_le(a)  __bswap_16(a)
#define h_u32_to_le(a)  __bswap_32(a)
#endif

// DAP_PKT_SIZE must be >= to what is used by the client (OpenOCD).
#define DAP_PKT_SIZE            CONFIG_ESP_DAP_TCP_MAX_PKT_SIZE
#define DAP_PKT_HDR_SIGNATURE   0x00504144   // "DAP\0" in LE
#define DAP_PKT_TYPE_REQUEST    0x01
#define DAP_PKT_TYPE_RESPONSE   0x02
#define CMSIS_DAP_TCP_MAX_ACTIVE 4

#ifndef MAX
#define MAX(a, b)               \
    ({ __typeof__(a) _a = (a);  \
       __typeof__(b) _b = (b);  \
       _a > _b ? _a : _b; })
#endif

// CMSIS-DAP requests are variable length. With CMSIS-DAP over USB, the
// transfer sizes are preserved by the USB stack. However, TCP/IP is stream
// oriented so we perform our own packetization to preserve the boundaries
// between each request. This short header is prepended to each CMSIS-DAP
// request and response before being sent over the socket. Little endian format
// is used for multibyte values.
struct cmsis_dap_tcp_packet_hdr {
    uint32_t signature;         // "DAP"
    uint16_t length;            // Not including header length.
    uint8_t packet_type;
    uint8_t reserved;           // Reserved for future use.
} __attribute__((__packed__));

#define DAP_TOTAL_PKT_SIZE sizeof(struct cmsis_dap_tcp_packet_hdr)+DAP_PKT_SIZE

struct msgbuf_t {
    uint8_t  data[3*DAP_TOTAL_PKT_SIZE];
    size_t   len;
};

// Keep TCP packet buffers in task-owned state instead of file-scope globals, so
// multiple server tasks would not share request/response scratch space.
struct cmsis_dap_tcp_state {
    struct msgbuf_t buf;
    uint8_t response[DAP_PKT_SIZE];
    uint8_t packet_buf[DAP_TOTAL_PKT_SIZE];
};

struct cmsis_dap_tcp_resources {
    int port;
    struct cmsis_dap_gpio_config gpio;
};

static struct cmsis_dap_tcp_resources active_resources[CMSIS_DAP_TCP_MAX_ACTIVE];
static portMUX_TYPE active_resources_mux = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------------------------------------------------------
// Use our own receive buffer to accumulate from the socket until a complete
// message packet is available.

static void msgbuf_init(struct msgbuf_t *buf)
{
    buf->len = 0;
}

// Read all data from the socket into our buffer.
static int msgbuf_add(struct msgbuf_t *buf, int sock)
{
    size_t space = sizeof(buf->data) - buf->len;
    if (space == 0)
        return -ENOSPC;

    ssize_t n = recv(sock, buf->data + buf->len, space, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;       // no new data
        perror("cmsis_dap_tcp: socket read error");
        return -1;
    }
    if (n == 0) {
        errno = ENOTCONN;   // connection closed
        return -1;
    }
    buf->len += (size_t)n;
    return 0;
}

// Read a complete CMSIS-DAP request packet from our buffer.
// After done using it, call msgbuf_consume(buf, total_len).
static int msgbuf_parse(struct msgbuf_t *buf,
        struct cmsis_dap_tcp_packet_hdr *hdr, const uint8_t **payload,
        size_t *payload_len, size_t *total_len)
{
    if (buf->len < sizeof(struct cmsis_dap_tcp_packet_hdr))
        return -EAGAIN;

    struct cmsis_dap_tcp_packet_hdr tmp;
    memcpy(&tmp, buf->data, sizeof(tmp));
    tmp.signature = le_to_h_u32(tmp.signature);
    tmp.length = le_to_h_u16(tmp.length);

    if (tmp.signature != DAP_PKT_HDR_SIGNATURE) {
        fprintf(stderr, "cmsis_dap_tcp: Invalid header signature 0x%08lx\n",
                tmp.signature);
        return -EINVAL;
    }

    if (tmp.packet_type != DAP_PKT_TYPE_REQUEST) {
        fprintf(stderr, "cmsis_dap_tcp: Unrecognized packet type 0x%02hx\n",
                tmp.packet_type);
        return -EINVAL;
    }

    if (buf->len < sizeof(*hdr) + tmp.length)
        return -EAGAIN;

    // A complete packet is available.
    *hdr = tmp;
    *payload = buf->data + sizeof(*hdr);
    if(payload_len) *payload_len = tmp.length;
    if(total_len) *total_len = tmp.length + sizeof(*hdr);
    LOG_DEBUG("Got CMSIS-DAP packet. Len %d", hdr->length);

    return 0;
}

// Discard data from the buffer.
static void msgbuf_consume(struct msgbuf_t *buf, size_t n)
{
    if(n > buf->len) n = buf->len;
    memmove(buf->data, buf->data + n, buf->len - n);
    buf->len -= n;

}

// ---------------------------------------------------------------------------

static int send_dap_response(struct cmsis_dap_tcp_state *state, int sock,
        const uint8_t *payload, uint16_t len)
{
    if (len > DAP_PKT_SIZE) {
        errno = EMSGSIZE;
        perror("cmsis_dap_tcp: response too large for buffer");
        return -1;
    }

    struct cmsis_dap_tcp_packet_hdr hdr;
    hdr.signature = h_u32_to_le(DAP_PKT_HDR_SIGNATURE);
    hdr.length = h_u16_to_le(len);
    hdr.packet_type = DAP_PKT_TYPE_RESPONSE;
    hdr.reserved = 0;

    memcpy(state->packet_buf, &hdr, sizeof(hdr));
    memcpy(state->packet_buf + sizeof(hdr), payload, len);

    size_t total_len = sizeof(hdr) + len;
    size_t sent = 0;

    while (sent < total_len) {
        ssize_t n = write(sock, state->packet_buf + sent, total_len - sent);
        if (n < 0) {
            if (errno == EINTR) {
                continue;   // retry
            }
            else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                perror("cmsis_dap_tcp: socket write would block, dropping "
                        "client");
                return -1;
            }
            else {
                perror("cmsis_dap_tcp: socket write error");
                return -1;
            }
        }
        sent += (size_t)n;
    }

    return 0;
}

static int process_dap_request(struct cmsis_dap_tcp_state *state, int sock,
            const uint8_t *request, uint16_t len)
{
    // DAP_ProcessCommand returns:
    //   number of bytes in response (lower 16 bits)
    //   number of bytes in request (upper 16 bits)
    int ret = DAP_ProcessCommand(request, state->response);
    int request_len __attribute__((unused)) = (ret>>16) & 0xFFFF;
    int response_len = ret & 0xFFFF;
    LOG_DEBUG("processed command. Request len: %d, response len: %d.",
            request_len, response_len);

    return send_dap_response(state, sock, state->response, response_len);
}

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) flags = 0;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void set_keepalives(int fd, const struct cmsis_dap_tcp_config *config)
{
    if (config && config->disable_keepalive)
        return;

#ifdef CONFIG_ESP_DAP_TCP_USE_KEEPALIVE
    // Use TCP keepalives to detect dead clients.
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));

    // Seconds between probes (Linux and ESP32)
    val = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val));

    // Number of probes to send before closing the connection.
    val = config && config->keepalive_timeout > 0 ?
            config->keepalive_timeout : CONFIG_ESP_DAP_TCP_KEEPALIVE_TIMEOUT;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val));

    LOG_DEBUG("cmsis_dap_tcp: Using TCP keepalives with %d second "
            "timeout.", val);
#endif
}

static void get_effective_gpio_config(const struct cmsis_dap_tcp_config *config,
        struct cmsis_dap_gpio_config *gpio)
{
    if (config && config->gpio) {
        *gpio = *config->gpio;
        return;
    }

    cmsis_dap_gpio_config_init(gpio);
}

static int reserve_resources(const struct cmsis_dap_tcp_config *config,
        int port, struct cmsis_dap_tcp_resources *resources)
{
    int slot = -1;
    resources->port = port;
    get_effective_gpio_config(config, &resources->gpio);

    portENTER_CRITICAL(&active_resources_mux);
    for (int i = 0; i < CMSIS_DAP_TCP_MAX_ACTIVE; i++) {
        if (active_resources[i].port == 0) {
            if (slot < 0)
                slot = i;
            continue;
        }
        if (active_resources[i].port == resources->port ||
                cmsis_dap_gpio_config_conflicts(&active_resources[i].gpio,
                        &resources->gpio)) {
            portEXIT_CRITICAL(&active_resources_mux);
            return -1;
        }
    }
    if (slot >= 0)
        active_resources[slot] = *resources;
    portEXIT_CRITICAL(&active_resources_mux);

    return slot;
}

static void release_resources(int slot)
{
    if (slot < 0)
        return;

    portENTER_CRITICAL(&active_resources_mux);
    active_resources[slot].port = 0;
    portEXIT_CRITICAL(&active_resources_mux);
}

BaseType_t cmsis_dap_tcp_start(const struct cmsis_dap_tcp_config *config,
        const char *task_name, TaskHandle_t *handle)
{
    return xTaskCreate(cmsis_dap_tcp_task,
            task_name ? task_name : "cmsis_dap_tcp_task",
            CMSIS_DAP_TCP_TASK_STACK_SIZE, (void *) config,
            CMSIS_DAP_TCP_TASK_PRIORITY, handle);
}

void cmsis_dap_tcp_task(void *arg)
{
    int listener_fd;
    const struct cmsis_dap_tcp_config *config = arg;
    int port = config && config->port > 0 ? config->port : CONFIG_ESP_DAP_TCP_PORT;
    struct cmsis_dap_tcp_resources resources;
    int resources_slot = reserve_resources(config, port, &resources);
    if (resources_slot < 0) {
        fprintf(stderr, "cmsis_dap_tcp: resource conflict on port or JTAG pins.\n");
        vTaskDelete(NULL);
        return;
    }

    cmsis_dap_gpio_config = config ? config->gpio : NULL;
    DAP_Setup();

#ifdef CONFIG_LWIP_IPV6
    // Dual stack to allow both IPv4 and IPv6 listeners.
    char ipstr[INET6_ADDRSTRLEN];
    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(port);

    listener_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if(listener_fd < 0) {
        perror("cmsis_dap_tcp: Failed to create listening socket.");
        release_resources(resources_slot);
        vTaskDelete(NULL);
        return;
    }

    int no = 0;
    if (setsockopt(listener_fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no)) <
            0) {
        perror("cmsis_dap_tcp: failed to disable IPV6_V6ONLY for socket");
    }
    LOG_DEBUG("Listening on IPv4/IPv6 socket.");
#else
    char ipstr[INET_ADDRSTRLEN];
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    listener_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(listener_fd < 0) {
        perror("cmsis_dap_tcp: Failed to create listening socket.");
        release_resources(resources_slot);
        vTaskDelete(NULL);
        return;
    }
    LOG_DEBUG("Listening on IPV4 socket.");
#endif

    int yes = 1;
    setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    if (bind(listener_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("cmsis_dap_tcp: failed to bind socket");
        close(listener_fd);
        release_resources(resources_slot);
        vTaskDelete(NULL);
        return;
    }

    if(listen(listener_fd, 1) < 0) {
        perror("cmsis_dap_tcp: failed to listen on socket");
        close(listener_fd);
        release_resources(resources_slot);
        vTaskDelete(NULL);
        return;
    }

    set_nonblocking(listener_fd);
    fprintf(stdout, "cmsis_dap_tcp: listening on port %d.\n", port);

    struct cmsis_dap_tcp_state *state = calloc(1, sizeof(*state));
    if (state == NULL) {
        perror("cmsis_dap_tcp: failed to allocate task state");
        close(listener_fd);
        release_resources(resources_slot);
        vTaskDelete(NULL);
        return;
    }

    msgbuf_init(&state->buf);

    // Only one active client at a time is allowed.
    int client_fd = -1;
    int run __attribute__((unused)) = 0;

    while (1) {
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listener_fd, &read_fds);
        if (client_fd >= 0) FD_SET(client_fd, &read_fds);
        int fdmax = MAX(client_fd, listener_fd);

        int sel = select(fdmax + 1, &read_fds, NULL, NULL, NULL);
        if (sel < 0) {
            if (errno == EINTR)
                continue;
            perror("cmsis_dap_tcp: select error");
            break;
        }

        LOG_DEBUG("run %d", ++run);

        // New connection?
        if (FD_ISSET(listener_fd, &read_fds)) {
            int new_fd = accept(listener_fd, (struct sockaddr*)&client_addr,
                    &addr_len);
            if(new_fd < 0) {
                if(errno != EAGAIN && errno != EWOULDBLOCK) {
                    // Just ignore error for now.
                    perror("cmsis_dap_tcp: accept error");
                }
            }
            else {
                if (client_fd >= 0) {
                    fprintf(stderr, "cmsis_dap_tcp: dropping new connection. "
                            "Another client is already connected.\n");
                    close(new_fd);
                    continue;   // restart select() loop
                }

                int client_port = 0;
                ipstr[0] = '\0';
                if(client_addr.ss_family == AF_INET) {
                    // IPv4
                    struct sockaddr_in* s = (void*) &client_addr;
                    inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof(ipstr));
                    client_port = ntohs(s->sin_port);
                }
#ifdef CONFIG_LWIP_IPV6
                else {
                    // IPv6
                    struct sockaddr_in6* s = (void*) &client_addr;
                    inet_ntop(AF_INET6, &s->sin6_addr, ipstr, sizeof(ipstr));
                    client_port = ntohs(s->sin6_port);
                }
#endif
                fprintf(stdout, "cmsis_dap_tcp: client connected %s:%d\n",
                        ipstr, client_port);
                fcntl(new_fd, F_SETFL, O_NONBLOCK);
                set_keepalives(new_fd, config);
                client_fd = new_fd;
                msgbuf_init(&state->buf);
                continue;   // restart select() loop
            }
        }

        // Data from client?
        if (client_fd >= 0 && FD_ISSET(client_fd, &read_fds)) {
            if (msgbuf_add(&state->buf, client_fd) < 0) {
                if(errno != ENOSPC) {
                    fprintf(stdout, "cmsis_dap_tcp: client disconnected.\n");
                    close(client_fd);
                    client_fd = -1;
                    continue;   // restart select() loop
                }
            }

            // Process all the DAP requests in our buffer.
            struct cmsis_dap_tcp_packet_hdr hdr;
            const uint8_t *payload;
            while (true) {
                size_t payload_len;
                size_t total_len;
                int ret = msgbuf_parse(&state->buf, &hdr, &payload, &payload_len,
                        &total_len);
                if(ret < 0)
                    break;

                ret = process_dap_request(state, client_fd, payload, payload_len);
                msgbuf_consume(&state->buf, total_len);

                // If we cannot process the request and response, just close
                // the connection.
                if(ret < 0) {
                    fprintf(stdout, "cmsis_dap_tcp: disconnecting.\n");
                    close(client_fd);
                    client_fd = -1;
                    break;
                }
            }
        }
    }

    fprintf(stdout, "cmsis_dap_tcp: shutting down.\n");
    if (client_fd >= 0) close(client_fd);
    close(listener_fd);
    free(state);
    release_resources(resources_slot);
    vTaskDelete(NULL);
}
