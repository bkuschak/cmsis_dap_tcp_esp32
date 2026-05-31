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

#if defined(CONFIG_ESP_UART_BRIDGE_PARITY_NONE)
#define UART_PARITY  UART_PARITY_DISABLE
#elif defined(CONFIG_ESP_UART_BRIDGE_PARITY_EVEN)
#define UART_PARITY  UART_PARITY_EVEN
#elif defined(CONFIG_ESP_UART_BRIDGE_PARITY_ODD)
#define UART_PARITY  UART_PARITY_ODD
#else
#error "Invalid setting for CONFIG_ESP_UART_BRIDGE_PARITY."
#endif

#if CONFIG_ESP_UART_BRIDGE_DATA_BITS == 7
#define UART_DATA_BITS      UART_DATA_7_BITS
#elif CONFIG_ESP_UART_BRIDGE_DATA_BITS == 8
#define UART_DATA_BITS      UART_DATA_8_BITS
#else
#error "Invalid setting for CONFIG_ESP_UART_BRIDGE_DATA_BITS."
#endif

#if CONFIG_ESP_UART_BRIDGE_STOP_BITS == 1
#define UART_STOP_BITS      UART_STOP_BITS_1
#elif CONFIG_ESP_UART_BRIDGE_STOP_BITS == 2
#define UART_STOP_BITS      UART_STOP_BITS_2
#else
#error "Invalid setting for CONFIG_ESP_UART_BRIDGE_STOP_BITS."
#endif

#ifndef MAX
#define MAX(a,b) \
({ __typeof__ (a) _a = (a); \
   __typeof__ (b) _b = (b); \
 _a > _b ? _a : _b; })
#endif

static const struct uart_bridge_config default_uart_bridge_config = {
    .port       = CONFIG_ESP_UART_BRIDGE_TCP_PORT,
#ifdef CONFIG_ESP_UART_BRIDGE_USE_KEEPALIVE
    .keepalive_timeout = CONFIG_ESP_UART_BRIDGE_KEEPALIVE_TIMEOUT,
#else
    .keepalive_timeout = 0,
#endif
    .uart_num   = CONFIG_ESP_UART_BRIDGE_UART_NUM,
#ifdef CONFIG_ESP_UART_BRIDGE_REMAP_PINS
    .txd_pin    = CONFIG_ESP_UART_BRIDGE_TXD_PIN,
    .rxd_pin    = CONFIG_ESP_UART_BRIDGE_RXD_PIN,
#else
    .txd_pin    = UART_PIN_NO_CHANGE,
    .rxd_pin    = UART_PIN_NO_CHANGE,
#endif
    .baud_rate  = CONFIG_ESP_UART_BRIDGE_BAUD_RATE,
    .data_bits  = UART_DATA_BITS,
    .parity     = UART_PARITY,
    .stop_bits  = UART_STOP_BITS,
};

void uart_bridge_task(void* arg)
{
    struct uart_bridge_config config =
        (arg != NULL) ? *(struct uart_bridge_config*)arg :
        default_uart_bridge_config;
    char buffer[BUFFER_SIZE];
    int ret;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(listen_fd < 0) {
        perror("UART bridge: Failed to create socket");
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
        perror("UART bridge: failed to bind socket");
        vTaskDelete(NULL);
        return;
    }
    ret = listen(listen_fd, 1);
    if(ret < 0) {
        perror("UART bridge: failed to listen on socket");
        vTaskDelete(NULL);
        return;
    }

    // Set up UART.
    ret = uart_driver_install(config.uart_num,
            UART_BUFFER_SIZE, UART_BUFFER_SIZE, 0, NULL, 0);
    if(ret != ESP_OK) {
        fprintf(stderr, "UART bridge: UART driver installation failed\n");
        vTaskDelete(NULL);
        return;
    }
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
        fprintf(stderr, "UART bridge: remapping UART_TX = GPIO_NUM_%d, "
                "UART_RX = GPIO_NUM_%d.\n", config.txd_pin, config.rxd_pin);
        ESP_ERROR_CHECK(uart_set_pin(config.uart_num,
                    config.txd_pin, config.rxd_pin,
                    UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    }

    char uart_addr[32];
    snprintf(uart_addr, sizeof(uart_addr), "/dev/uart/%d", config.uart_num);

    fprintf(stdout, "UART bridge: listening on port %d for UART%d.\n",
            config.port, config.uart_num);

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
            perror("UART bridge: select error");
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
                    perror("UART bridge: accept error");
                }
            }
            else {
                if(client_fd < 0) {
                    // New client.
                    fcntl(new_fd, F_SETFL, O_NONBLOCK);
                    client_fd = new_fd;
                    fprintf(stdout, "UART bridge: client connected %s:%d\n",
                            inet_ntoa(client_addr.sin_addr),
                            ntohs(client_addr.sin_port));

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
                        perror("UART bridge: failed opening UART");
                        close(client_fd);
                        client_fd = -1;
                    }
                    uart_vfs_dev_use_driver(config.uart_num);

                    int flags = fcntl(uart_fd, F_GETFL, 0);
                    fcntl(uart_fd, F_SETFL, flags | O_NONBLOCK);

                    // Restart select() loop.
                    continue;
                }
                else {
                    fprintf(stderr, "UART bridge: dropping new connection. "
                            "Another client is already connected.\n");
                    close(new_fd);
                }
            }
        }

        // Handle client socket.
        if(client_fd > 0 && FD_ISSET(client_fd, &read_fds)) {
            ret = recv(client_fd, buffer, sizeof(buffer)-1, 0);
            if(ret == 0 ||
              (ret < 0 && (errno == ECONNABORTED || errno == ENOTCONN))) {
                // Client has disconnected.
                fprintf(stdout, "UART bridge: client disconnected.\n");
                close(client_fd);
                close(uart_fd);
                client_fd = -1;
                uart_fd = -1;
                continue;       // restart select() loop
            }
            else if(ret < 0) {
                if(errno != EAGAIN && errno != EWOULDBLOCK)
                    perror("UART bridge: socket read error");
            }
            else {
                write(uart_fd, buffer, ret);
            }
        }

        // Handle UART.
        if(uart_fd > 0 && FD_ISSET(uart_fd, &read_fds)) {
            ret = read(uart_fd, buffer, sizeof(buffer)-1);
            if(ret <= 0) {
                if(errno != EAGAIN && errno != EWOULDBLOCK)
                    perror("UART bridge: UART read error");
            }
            else {
                send(client_fd, buffer, ret, 0);
            }
        }
    }

    fprintf(stdout, "UART bridge: shutting down.\n");

    if(client_fd >= 0)
        close(client_fd);
    if(uart_fd >= 0)
        close(uart_fd);
    close(listen_fd);
    vTaskDelete(NULL);
}
