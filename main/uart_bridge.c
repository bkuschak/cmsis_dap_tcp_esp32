/*
 * SPDX-FileCopyrightText: Brian Kuschak <bkuschak@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 *
 * UART to TCP/IP bridge.
 *
 * Runs a TCP/IP server that connects to an ESP32 UART. The user may physically
 * wire this UART to a target board's UART for remote access to the target's
 * console.
 *
 * On the host machine, 'socat' can be used to connect to this bridge and
 * create a local pty file that looks like a serial port:
 *
 *     socat TCP:192.168.1.5:4442 PTY,link=/tmp/tty_uart,raw,echo=0
 *
 * The pty can then be opened by any serial terminal program:
 *
 *     screen /tmp/tty_uart
 *
 * The standalone app default is fixed at build time and can be adjusted by
 * these menuconfig options:
 *
 *     CONFIG_ESP_UART_BRIDGE_UART_NUM
 *     CONFIG_ESP_UART_BRIDGE_BAUD_RATE
 *     CONFIG_ESP_UART_BRIDGE_DATA_BITS
 *     CONFIG_ESP_UART_BRIDGE_PARITY
 *     CONFIG_ESP_UART_BRIDGE_STOP_BITS
 *
 * This code supports multiple UART bridges operating simultaneously on
 * different TCP/IP ports, if the hardware has enough available UARTs.
 */

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "errno.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "netdb.h"
#include <stdio.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/unistd.h>
#include "uart_bridge.h"

#define BUFFER_SIZE         512
#define UART_BUFFER_SIZE    512

#define UART_BRIDGE_TASK_STACK_SIZE     4096
#define UART_BRIDGE_TASK_PRIORITY       5
#define UART_BRIDGE_MAX_TASKS           4

#ifndef MAX
#define MAX(a,b) \
({ __typeof__ (a) _a = (a); \
   __typeof__ (b) _b = (b); \
 _a > _b ? _a : _b; })
#endif

// Per-task state. A local (not heap-allocated) struct in uart_bridge_task()
// is fine here -- unlike cmsis_dap_tcp_state, these buffers are small enough
// for the task's own stack, and the struct only needs to live as long as
// the task's function frame, which is its entire lifetime.
struct uart_bridge_state {
    const struct uart_bridge_config *config;
    char buffer[BUFFER_SIZE];
    char client_ip_str[INET_ADDRSTRLEN];
    int client_port;
    bool client_connected;
    unsigned long count_rx;
    unsigned long count_tx;
};

// Registry of running tasks, for conflict detection (port/UART already in
// use) and for uart_bridge_print_status() to enumerate active instances.
static struct uart_bridge_state *task_states[UART_BRIDGE_MAX_TASKS];
static portMUX_TYPE task_states_mux = portMUX_INITIALIZER_UNLOCKED;

static int reserve_resources(struct uart_bridge_state *state)
{
    int slot = -1;

    portENTER_CRITICAL(&task_states_mux);
    for (int i = 0; i < UART_BRIDGE_MAX_TASKS; i++) {
        if (task_states[i] == NULL) {
            if (slot < 0)
                slot = i;
        } else if (task_states[i]->config->port == state->config->port ||
                task_states[i]->config->uart_num == state->config->uart_num) {
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

void uart_bridge_print_status(void)
{
    struct {
        bool active;
        bool client_connected;
        int txd_pin;
        int rxd_pin;
        int port;
        int uart_num;
        int client_port;
        char client_ip_str[INET_ADDRSTRLEN];
        unsigned long count_rx;
        unsigned long count_tx;
    } snapshot[UART_BRIDGE_MAX_TASKS] = {0};

    // Snapshot under the lock, then print afterward -- printf() is too slow
    // to call while holding a critical section.
    portENTER_CRITICAL(&task_states_mux);
    for (int i = 0; i < UART_BRIDGE_MAX_TASKS; i++) {
        struct uart_bridge_state *state = task_states[i];
        if (state == NULL)
            continue;
        snapshot[i].active = true;
        snapshot[i].client_connected = state->client_connected;
        snapshot[i].txd_pin = state->config->txd_pin;
        snapshot[i].rxd_pin = state->config->rxd_pin;
        snapshot[i].port = state->config->port;
        snapshot[i].uart_num = state->config->uart_num;
        snapshot[i].client_port = state->client_port;
        snapshot[i].count_rx = state->count_rx;
        snapshot[i].count_tx = state->count_tx;
        memcpy(snapshot[i].client_ip_str, state->client_ip_str,
                sizeof(snapshot[i].client_ip_str));
    }
    portEXIT_CRITICAL(&task_states_mux);

    bool any = false;
    for (int i = 0; i < UART_BRIDGE_MAX_TASKS; i++) {
        if (!snapshot[i].active)
            continue;
        any = true;
        if (snapshot[i].client_connected) {
            fprintf(stdout, "UART%d bridge: port %d connected to '%s:%d'. "
                    "GPIOs: TX=%d RX=%d. Bytes: TX=%lu RX=%lu.\n",
                    snapshot[i].uart_num, snapshot[i].port,
                    snapshot[i].client_ip_str, snapshot[i].client_port,
                    snapshot[i].txd_pin, snapshot[i].rxd_pin,
                    snapshot[i].count_tx, snapshot[i].count_rx);
        } else {
            fprintf(stdout, "UART%d bridge: Listening on port %d. GPIOs: TX=%d"
                    " RX=%d.\n",
                    snapshot[i].uart_num, snapshot[i].port,
                    snapshot[i].txd_pin, snapshot[i].rxd_pin);
        }
    }
    if (!any)
        fprintf(stdout, "UART bridge: not running.\n");
}

static void uart_bridge_task(void* arg)
{
    struct uart_bridge_config config = *(struct uart_bridge_config*)arg;
    struct uart_bridge_state state = {0};
    state.config = &config;
    int ret;

    int slot = reserve_resources(&state);
    if (slot < 0) {
        fprintf(stderr, "UART%d bridge: resource conflict on port or UART.\n",
                config.uart_num);
        vTaskDelete(NULL);
        return;
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(listen_fd < 0) {
        fprintf(stderr, "UART%d bridge: Failed to create socket: %s\n",
                config.uart_num, strerror(errno));
        release_resources(slot);
        vTaskDelete(NULL);
        return;
    }

    // Set socket as non-blocking.
    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);

    // Bind server socket and listen.
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(config.port);
    ret = bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if(ret < 0) {
        fprintf(stderr, "UART%d bridge: failed to bind socket: %s\n",
                config.uart_num, strerror(errno));
        release_resources(slot);
        vTaskDelete(NULL);
        return;
    }
    ret = listen(listen_fd, 1);
    if(ret < 0) {
        fprintf(stderr, "UART%d bridge: failed to listen on socket: %s\n",
                config.uart_num, strerror(errno));
        release_resources(slot);
        vTaskDelete(NULL);
        return;
    }

    // Set up UART.
    ret = uart_driver_install(config.uart_num,
            UART_BUFFER_SIZE, UART_BUFFER_SIZE, 0, NULL, 0);
    if(ret != ESP_OK) {
        fprintf(stderr, "UART%d bridge: UART driver installation failed\n",
                config.uart_num);
        release_resources(slot);
        vTaskDelete(NULL);
        return;
    }
    uart_vfs_dev_register();

    uart_config_t uart_config = {
        .baud_rate  = config.baud_rate,
        .data_bits  = config.data_bits,
        .parity     = config.parity,
        .stop_bits  = config.stop_bits,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(config.uart_num,
                &uart_config));

    if(config.txd_pin != UART_PIN_NO_CHANGE ||
       config.rxd_pin != UART_PIN_NO_CHANGE) {
        fprintf(stderr, "UART%d bridge: GPIOs: TX=%d RX=%d.\n",
                config.uart_num, config.txd_pin,
                config.rxd_pin);

        // Disable any GPIO output drive on these pins before handing them
        // to the UART peripheral.
        if (config.rxd_pin != UART_PIN_NO_CHANGE)
            ESP_ERROR_CHECK(gpio_set_direction(config.rxd_pin,
                        GPIO_MODE_INPUT));
        if (config.txd_pin != UART_PIN_NO_CHANGE)
            ESP_ERROR_CHECK(gpio_set_direction(config.txd_pin,
                        GPIO_MODE_INPUT));
        ESP_ERROR_CHECK(uart_set_pin(config.uart_num,
                    config.txd_pin, config.rxd_pin,
                    UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    }
    uart_vfs_dev_use_driver(config.uart_num);

    char uart_addr[32];
    snprintf(uart_addr, sizeof(uart_addr), "/dev/uart/%d", config.uart_num);

    fprintf(stdout, "UART%d bridge: listening on port %d.\n",
            config.uart_num, config.port);

    // Select() loop blocks until activity on sockets or UART.
    struct sockaddr_in client_addr;
    int client_fd = -1;
    int uart_fd = -1;
    while (1) {
        fd_set read_fds;
        FD_ZERO(&read_fds);

        // Add listening socket and client socket to read_fds.
        FD_SET(listen_fd, &read_fds);
        if(client_fd >= 0)
            FD_SET(client_fd, &read_fds);
        if(uart_fd >= 0)
            FD_SET(uart_fd, &read_fds);
        int max_fd = MAX(listen_fd, MAX(client_fd, uart_fd));

        // Blocking call to select.
        int activity = select(max_fd+1, &read_fds, NULL, NULL, NULL);
        if (activity < 0) {
            //ESP_LOGE(TAG, "select failed: errno %d", errno);
            fprintf(stderr, "UART%d bridge: select error: %s\n",
                    config.uart_num, strerror(errno));
            break;
        }

        // Handle new connections.
        if (FD_ISSET(listen_fd, &read_fds)) {
            socklen_t addr_len = sizeof(client_addr);
            int new_fd = accept(listen_fd, (struct sockaddr*)&client_addr,
                    &addr_len);
            if(new_fd < 0) {
                if(errno != EAGAIN && errno != EWOULDBLOCK) {
                    // Just ignore error for now.
                    fprintf(stderr, "UART%d bridge: accept error: %s\n",
                            config.uart_num, strerror(errno));
                }
            }
            else {
                if(client_fd < 0) {
                    // New client.
                    fcntl(new_fd, F_SETFL, O_NONBLOCK);
                    client_fd = new_fd;
                    inet_ntop(AF_INET, &client_addr.sin_addr, state.client_ip_str,
                            sizeof(state.client_ip_str));
                    state.client_port = ntohs(client_addr.sin_port);
                    fprintf(stdout, "UART%d bridge: client connected %s:%d\n",
                            config.uart_num, state.client_ip_str,
                            state.client_port);

                    if(config.keepalive_timeout > 0) {
                    // Use TCP keepalives to detect dead clients.
                    int val = 1;
                    setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &val,
                            sizeof(val));
                    // Seconds between probes (Linux and ESP32)
                    val = 1;
                    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPIDLE, &val,
                            sizeof(val));
                    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPINTVL, &val,
                            sizeof(val));
                    // Number of probes to send before closing the connection.
                    val = config.keepalive_timeout;
                    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPCNT, &val,
                            sizeof(val));
                    }

                    // Open UART.
                    uart_fd = open(uart_addr, O_RDWR);
                    if(uart_fd < 0) {
                        fprintf(stderr, "UART%d bridge: failed opening UART: %s\n",
                                config.uart_num, strerror(errno));
                        close(client_fd);
                        client_fd = -1;
                    }

                    int flags = fcntl(uart_fd, F_GETFL, 0);
                    fcntl(uart_fd, F_SETFL, flags | O_NONBLOCK);

                    state.count_rx = 0;
                    state.count_tx = 0;
                    state.client_connected = true;

                    // Restart select() loop.
                    continue;
                }
                else {
                    fprintf(stderr, "UART%d bridge: dropping new connection. "
                            "Another client is already connected.\n",
                            config.uart_num);
                    close(new_fd);
                }
            }
        }

        // Handle client socket.
        if(client_fd > 0 && FD_ISSET(client_fd, &read_fds)) {
            ret = recv(client_fd, state.buffer, sizeof(state.buffer)-1, 0);
            if(ret == 0 ||
              (ret < 0 && (errno == ECONNABORTED || errno == ENOTCONN))) {
                // Client has disconnected.
                fprintf(stdout, "UART%d bridge: client disconnected.\n",
                        config.uart_num);
                close(client_fd);
                close(uart_fd);
                client_fd = -1;
                uart_fd = -1;
                state.count_rx = 0;
                state.count_tx = 0;
                state.client_connected = false;
                continue;       // restart select() loop
            }
            else if(ret < 0) {
                if(errno != EAGAIN && errno != EWOULDBLOCK)
                    fprintf(stderr, "UART%d bridge: socket read error: %s\n",
                            config.uart_num, strerror(errno));
            }
            else {
                write(uart_fd, state.buffer, ret);
                state.count_tx += ret;
            }
        }

        // Handle UART.
        if(uart_fd > 0 && FD_ISSET(uart_fd, &read_fds)) {
            ret = read(uart_fd, state.buffer, sizeof(state.buffer)-1);
            if(ret <= 0) {
                if(errno != EAGAIN && errno != EWOULDBLOCK)
                    fprintf(stderr, "UART%d bridge: UART read error: %s\n",
                            config.uart_num, strerror(errno));
            }
            else {
                state.count_rx += ret;
                send(client_fd, state.buffer, ret, 0);
            }
        }
    }

    fprintf(stdout, "UART%d bridge: shutting down.\n", config.uart_num);
    state.client_connected = false;
    state.count_rx = 0;
    state.count_tx = 0;

    if(client_fd >= 0)
        close(client_fd);
    if(uart_fd >= 0)
        close(uart_fd);
    close(listen_fd);
    release_resources(slot);
    vTaskDelete(NULL);
}

BaseType_t uart_bridge_start(const struct uart_bridge_config *config,
        const char *task_name, TaskHandle_t *handle)
{
    if(config == NULL)
        return pdFAIL;

    return xTaskCreate(uart_bridge_task,
            task_name ? task_name : "uart_bridge_task",
            UART_BRIDGE_TASK_STACK_SIZE, (void *) config,
            UART_BRIDGE_TASK_PRIORITY, handle);
}

