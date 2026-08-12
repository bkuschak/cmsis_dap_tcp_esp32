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

#include "DAP_config.h"
#include "DAP.h"
#include "cmsis_dap_tcp.h"
#include "tls_transport.h"

#ifdef CONFIG_ESP_DAP_TCP_DEBUG_PRINTING
#define LOG_DEBUG(...) \
{ \
    fprintf(stderr, "cmsis_dap_tcp %d: ", \
            task_state->config->instance); \
    fprintf(stderr, ##__VA_ARGS__); \
    fprintf(stderr, "\n"); \
}

#else
#define LOG_DEBUG(...) { }
#endif

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

#ifdef CONFIG_LWIP_IPV6
#define MAX_INET_ADDRSTRLEN     INET6_ADDRSTRLEN
#else
#define MAX_INET_ADDRSTRLEN     INET_ADDRSTRLEN
#endif

// DAP_PKT_SIZE must be >= to what is used by the client (OpenOCD).
#define DAP_PKT_SIZE            CONFIG_ESP_DAP_TCP_MAX_PKT_SIZE
#define DAP_PKT_HDR_SIGNATURE   0x00504144   // "DAP\0" in LE
#define DAP_PKT_TYPE_REQUEST    0x01
#define DAP_PKT_TYPE_RESPONSE   0x02
#define CMSIS_DAP_TCP_MAX_TASKS 4

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

#define DAP_TOTAL_PKT_SIZE (sizeof(struct cmsis_dap_tcp_packet_hdr)+DAP_PKT_SIZE)

struct msgbuf_t {
    uint8_t  data[3*DAP_TOTAL_PKT_SIZE];
    size_t   len;
};

// Keep TCP packet buffers in task-owned state instead of file-scope globals,
// so multiple server tasks would not share request/response scratch space.
// Also doubles as the per-task registry entry used for port/GPIO conflict
// detection, and for the "status" command below.
struct cmsis_dap_tcp_state {
    const struct cmsis_dap_tcp_config *config;
    DAP_Data_t dap_data;
    struct msgbuf_t buf;
    uint8_t response[DAP_PKT_SIZE];
    uint8_t packet_buf[DAP_TOTAL_PKT_SIZE];
    char client_ip_str[MAX_INET_ADDRSTRLEN];
    int client_port;
    bool client_connected;
};

static struct cmsis_dap_tcp_state *task_states[CMSIS_DAP_TCP_MAX_TASKS];
static portMUX_TYPE task_states_mux = portMUX_INITIALIZER_UNLOCKED;
static __thread struct cmsis_dap_tcp_state *task_state;

// ---------------------------------------------------------------------------
// Use our own receive buffer to accumulate from the socket until a complete
// message packet is available.

static void msgbuf_init(struct msgbuf_t *buf)
{
    buf->len = 0;
}

// Read all data from the socket into our buffer.
static int msgbuf_add(struct msgbuf_t *buf, transport_handle_t t)
{
    size_t space = sizeof(buf->data) - buf->len;
    if (space == 0)
        return -ENOSPC;

    ssize_t n = transport_read(t, buf->data + buf->len, space);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;       // no new data
        fprintf(stderr, "cmsis_dap_tcp %d: socket read error: %s\n",
                task_state->config->instance, strerror(errno));
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
        fprintf(stderr, "cmsis_dap_tcp %d: Invalid header signature 0x%08lx\n",
                task_state->config->instance, tmp.signature);
        return -EINVAL;
    }

    if (tmp.packet_type != DAP_PKT_TYPE_REQUEST) {
        fprintf(stderr, "cmsis_dap_tcp %d: Unrecognized packet type 0x%02hx\n",
                task_state->config->instance, tmp.packet_type);
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

static int send_dap_response(struct cmsis_dap_tcp_state *state, transport_handle_t t,
        const uint8_t *payload, uint16_t len)
{
    if (len > DAP_PKT_SIZE) {
        errno = EMSGSIZE;
        fprintf(stderr, "cmsis_dap_tcp %d: response too large for buffer: %s\n",
                task_state->config->instance, strerror(errno));
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
    if (transport_write_all(t, state->packet_buf, total_len) != 0) {
        fprintf(stderr, "cmsis_dap_tcp %d: socket write error: %s\n",
                task_state->config->instance, strerror(errno));
        return -1;
    }

    return 0;
}

static int process_dap_request(struct cmsis_dap_tcp_state *state, transport_handle_t t,
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

    return send_dap_response(state, t, state->response, response_len);
}

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) flags = 0;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int reserve_resources(struct cmsis_dap_tcp_state *state)
{
    int slot = -1;

    portENTER_CRITICAL(&task_states_mux);
    for (int i = 0; i < CMSIS_DAP_TCP_MAX_TASKS; i++) {
        if (task_states[i] == NULL) {
            if (slot < 0)
                slot = i;
        }
        else if (task_states[i]->config->port == state->config->port ||
                cmsis_dap_gpio_config_conflicts(&task_states[i]->config->gpio,
                        &state->config->gpio)) {
            portEXIT_CRITICAL(&task_states_mux);
            return -1;
        }
    }
    if (slot >= 0)
        task_states[slot] = state;
    portEXIT_CRITICAL(&task_states_mux);

    return slot;
}

static void release_resources(int slot)
{
    if (slot < 0)
        return;

    portENTER_CRITICAL(&task_states_mux);
    task_states[slot] = NULL;
    portEXIT_CRITICAL(&task_states_mux);
}

// Format only GPIOs that are actually assigned to this instance, with a
// leading space: " SWCLK=0 SWDIO=1 TDI=9 TDO=10 NTRST=7 NRESET=2 LED=8".
static void format_gpios(char *buf, size_t bufsize,
        const struct cmsis_dap_gpio_config *gpio)
{
    struct { const char *name; int value; } pins[] = {
        { "SWCLK",  gpio->swclk_tck },
        { "SWDIO",  gpio->swdio_tms },
        { "TDI",    gpio->tdi },
        { "TDO",    gpio->tdo },
        { "NTRST",  gpio->ntrst },
        { "NRESET", gpio->nreset },
        { "LED",    gpio->led },
    };
    size_t pos = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        if (pins[i].value < 0)
            continue;
        int n = snprintf(buf + pos, bufsize - pos, " %s=%d",
                pins[i].name, pins[i].value);
        if (n < 0 || (size_t)n >= bufsize - pos)
            break;
        pos += (size_t)n;
    }
}

// "JTAG", "SWD", "JTAG/SWD", or "none" -- which protocols are actually
// usable for this instance (compiled in AND this instance's pins are
// valid). Same logic as DAP_SWD_AVAILABLE()/DAP_JTAG_AVAILABLE(), but
// takes an explicit gpio config instead of relying on the thread-local
// cmsis_dap_gpio_config, since cmsis_dap_print_status() runs from the
// console task, not any instance's own task.
static const char* protocol_str(const struct cmsis_dap_gpio_config *gpio)
{
    bool swd = (DAP_SWD != 0) && gpio->swclk_tck >= 0 && gpio->swdio_tms >= 0;
    bool jtag = (DAP_JTAG != 0) && gpio->swclk_tck >= 0 && gpio->swdio_tms >= 0 &&
            gpio->tdi >= 0 && gpio->tdo >= 0;
    if (jtag && swd) return "JTAG/SWD";
    if (jtag) return "JTAG";
    if (swd) return "SWD";
    return "none";
}

static const char* drive_strength_str(int drive_strength)
{
    switch (drive_strength) {
        case 0: return "weakest";
        case 1: return "weak";
        case 2: return "medium";
        case 3: return "strong";
        default: return "unknown";
    }
}

void cmsis_dap_print_status(FILE *out)
{
    struct {
        bool active;
        bool client_connected;
        int instance;
        int port;
        int client_port;
        char client_ip_str[MAX_INET_ADDRSTRLEN];
        struct cmsis_dap_gpio_config gpio;
    } snapshot[CMSIS_DAP_TCP_MAX_TASKS] = {0};

    // Snapshot under the lock, then print afterward -- printf() is too slow
    // to call while holding a critical section.
    portENTER_CRITICAL(&task_states_mux);
    for (int i = 0; i < CMSIS_DAP_TCP_MAX_TASKS; i++) {
        struct cmsis_dap_tcp_state *state = task_states[i];
        if (state == NULL)
            continue;
        snapshot[i].active = true;
        snapshot[i].client_connected = state->client_connected;
        snapshot[i].instance = state->config->instance;
        snapshot[i].port = state->config->port;
        snapshot[i].client_port = state->client_port;
        memcpy(snapshot[i].client_ip_str, state->client_ip_str,
                sizeof(snapshot[i].client_ip_str));
        snapshot[i].gpio = state->config->gpio;
    }
    portEXIT_CRITICAL(&task_states_mux);

    for (int i = 0; i < CMSIS_DAP_TCP_MAX_TASKS; i++) {
        if (!snapshot[i].active)
            continue;

        char gpio_str[80];
        format_gpios(gpio_str, sizeof(gpio_str), &snapshot[i].gpio);
        const char *proto = protocol_str(&snapshot[i].gpio);
        const char *drive = drive_strength_str(snapshot[i].gpio.drive_strength);

        fprintf(out, "cmsis_dap_tcp %d: %s, port %d. GPIO (%s):%s\n",
                snapshot[i].instance, proto, snapshot[i].port, drive,
                gpio_str);
        if (snapshot[i].client_connected) {
            fprintf(out, "cmsis_dap_tcp %d: connected to client '%s:%d'.\n",
                    snapshot[i].instance, snapshot[i].client_ip_str,
                    snapshot[i].client_port);
        }
    }
}

BaseType_t cmsis_dap_tcp_start(const struct cmsis_dap_tcp_config *config,
        const char *task_name, TaskHandle_t *handle)
{
    if(config == NULL)
        return pdFAIL;

    return xTaskCreate(cmsis_dap_tcp_task,
            task_name ? task_name : "cmsis_dap_tcp_task",
            CMSIS_DAP_TCP_TASK_STACK_SIZE, (void *) config,
            CMSIS_DAP_TCP_TASK_PRIORITY, handle);
}

void cmsis_dap_tcp_task(void *arg)
{
    int listener_fd;
    const struct cmsis_dap_tcp_config *config = arg;

    struct cmsis_dap_tcp_state *state = calloc(1, sizeof(*state));
    if (state == NULL) {
        fprintf(stderr, "cmsis_dap_tcp %d: failed to allocate task state: %s\n",
                config->instance, strerror(errno));
        vTaskDelete(NULL);
        return;
    }
    state->config = config;
    task_state = state;

    int resources_slot = reserve_resources(state);
    if (resources_slot < 0) {
        fprintf(stderr, "cmsis_dap_tcp %d: resource conflict on port or "
                "JTAG pins.\n", task_state->config->instance);
        free(state);
        vTaskDelete(NULL);
        return;
    }

    cmsis_dap_gpio_config = &config->gpio;
    DAP_Data = &state->dap_data;
    char gpio_str[80];
    format_gpios(gpio_str, sizeof(gpio_str), &config->gpio);
    DAP_Setup();

#ifdef CONFIG_LWIP_IPV6
    // Dual stack to allow both IPv4 and IPv6 listeners.
    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(config->port);

    listener_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if(listener_fd < 0) {
        fprintf(stderr, "cmsis_dap_tcp %d: Failed to create listening "
                "socket: %s\n", task_state->config->instance, strerror(errno));
        release_resources(resources_slot);
        free(state);
        vTaskDelete(NULL);
        return;
    }

    int no = 0;
    if (setsockopt(listener_fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no)) <
            0) {
        fprintf(stderr, "cmsis_dap_tcp %d: failed to disable IPV6_V6ONLY "
                "for socket: %s\n", task_state->config->instance, strerror(errno));
    }
    LOG_DEBUG("Listening on IPv4/IPv6 socket.");
#else
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config->port);

    listener_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(listener_fd < 0) {
        fprintf(stderr, "cmsis_dap_tcp %d: Failed to create listening "
                "socket: %s\n", task_state->config->instance, strerror(errno));
        release_resources(resources_slot);
        free(state);
        vTaskDelete(NULL);
        return;
    }
    LOG_DEBUG("Listening on IPV4 socket.");
#endif

    int yes = 1;
    setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    if (bind(listener_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "cmsis_dap_tcp %d: failed to bind socket: %s\n",
                task_state->config->instance, strerror(errno));
        close(listener_fd);
        release_resources(resources_slot);
        free(state);
        vTaskDelete(NULL);
        return;
    }

    if(listen(listener_fd, 1) < 0) {
        fprintf(stderr, "cmsis_dap_tcp %d: failed to listen on socket: %s\n",
                task_state->config->instance, strerror(errno));
        close(listener_fd);
        release_resources(resources_slot);
        free(state);
        vTaskDelete(NULL);
        return;
    }

    set_nonblocking(listener_fd);
    fprintf(stdout, "cmsis_dap_tcp %d: %s, port %d. GPIO (%s):%s\n",
            task_state->config->instance, protocol_str(&config->gpio),
            config->port, drive_strength_str(config->gpio.drive_strength),
            gpio_str);

    msgbuf_init(&state->buf);

    // Only one active client at a time is allowed.
    transport_handle_t client_t = NULL;
    int run __attribute__((unused)) = 0;
    state->client_connected = false;
    state->client_ip_str[0] = '\0';

    while (1) {
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listener_fd, &read_fds);
        if (client_t != NULL) FD_SET(transport_fd(client_t), &read_fds);
        int fdmax = MAX(client_t != NULL ? transport_fd(client_t) : -1, listener_fd);

        // If TLS already has a full request buffered above the socket
        // layer, don't block in select() waiting for more raw bytes that
        // may never come (the client is waiting on our response) --
        // poll instead so the pending data gets processed below.
        struct timeval zero_tv = {0, 0};
        bool pending = client_t != NULL && transport_has_pending(client_t);
        int sel = select(fdmax + 1, &read_fds, NULL, NULL,
                pending ? &zero_tv : NULL);
        if (sel < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "cmsis_dap_tcp %d: select error: %s\n",
                    task_state->config->instance, strerror(errno));
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
                    fprintf(stderr, "cmsis_dap_tcp %d: accept error: %s\n",
                            task_state->config->instance, strerror(errno));
                }
            }
            else {
                if (client_t != NULL) {
                    fprintf(stderr, "cmsis_dap_tcp %d: dropping new "
                            "connection. Another client is already "
                            "connected.\n", task_state->config->instance);
                    close(new_fd);
                    continue;   // restart select() loop
                }

                state->client_port = 0;
                state->client_ip_str[0] = '\0';
                if(client_addr.ss_family == AF_INET) {
                    // IPv4
                    struct sockaddr_in* s = (void*) &client_addr;
                    inet_ntop(AF_INET, &s->sin_addr, state->client_ip_str,
                            sizeof(state->client_ip_str));
                    state->client_port = ntohs(s->sin_port);
                }
#ifdef CONFIG_LWIP_IPV6
                else {
                    // IPv6
                    struct sockaddr_in6* s = (void*) &client_addr;
                    inet_ntop(AF_INET6, &s->sin6_addr, state->client_ip_str,
                            sizeof(state->client_ip_str));
                    state->client_port = ntohs(s->sin6_port);
                }
#endif
                fprintf(stdout, "cmsis_dap_tcp %d: client connected %s:%d\n",
                        task_state->config->instance, state->client_ip_str,
                        state->client_port);
                transport_handle_t new_t = transport_wrap(new_fd);
                if (new_t == NULL) {
                    fprintf(stderr, "cmsis_dap_tcp %d: transport setup "
                            "failed, dropping client.\n",
                            task_state->config->instance);
                    continue;   // restart select() loop; new_fd already closed
                }
                transport_set_keepalives(new_fd, config->keepalive_timeout);
                transport_set_nonblocking(new_t);
                client_t = new_t;
                msgbuf_init(&state->buf);
                state->client_connected = true;
                continue;   // restart select() loop
            }
        }

        // Data from client?
        if (client_t != NULL && (FD_ISSET(transport_fd(client_t), &read_fds) ||
                transport_has_pending(client_t))) {
            int add_ret = msgbuf_add(&state->buf, client_t);
            if (add_ret < 0) {
                if(add_ret != -ENOSPC) {
                    fprintf(stdout, "cmsis_dap_tcp %d: client disconnected.\n",
                            task_state->config->instance);
                    transport_close(client_t);
                    client_t = NULL;
                    state->client_connected = false;
                    state->client_ip_str[0] = '\0';
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

                ret = process_dap_request(state, client_t, payload, payload_len);
                msgbuf_consume(&state->buf, total_len);

                // If we cannot process the request and response, just close
                // the connection.
                if(ret < 0) {
                    fprintf(stdout, "cmsis_dap_tcp %d: disconnecting.\n",
                            task_state->config->instance);
                    transport_close(client_t);
                    client_t = NULL;
                    state->client_connected = false;
                    state->client_ip_str[0] = '\0';
                    break;
                }
            }
        }
    }

    fprintf(stdout, "cmsis_dap_tcp %d: shutting down.\n", task_state->config->instance);
    state->client_connected = false;

    if (client_t != NULL) transport_close(client_t);
    close(listener_fd);
    release_resources(resources_slot);
    free(state);
    vTaskDelete(NULL);
}
