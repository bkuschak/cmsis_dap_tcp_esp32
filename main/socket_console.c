/*
 * SPDX-FileCopyrightText: Brian Kuschak <bkuschak@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Console commands over TCP/IP. Unlike this project's other TCP services,
 * supports multiple simultaneous client connections -- each gets its own
 * task, esp_linenoise instance, and command_context (with its own private
 * argtable state), so connections never share mutable state with each
 * other or with the serial console.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <sys/unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "netdb.h"

#include "commands.h"
#include "socket_console.h"

#define LISTEN_TASK_STACK_SIZE     4096
#define LISTEN_TASK_PRIORITY       5
#define CONNECTION_TASK_STACK_SIZE 4096
#define CONNECTION_TASK_PRIORITY   5
#define LISTEN_BACKLOG             4

struct connection_args {
    int client_fd;
    struct socket_console_config config;
    char client_ip_str[INET_ADDRSTRLEN];
    int client_port;
};

static void connection_task(void *arg)
{
    struct connection_args *args = (struct connection_args *)arg;
    int client_fd = args->client_fd;
    struct socket_console_config config = args->config;
    char client_ip_str[INET_ADDRSTRLEN];
    int client_port = args->client_port;
    memcpy(client_ip_str, args->client_ip_str, sizeof(client_ip_str));
    free(args);

    if (config.keepalive_timeout > 0) {
        int val = 1;
        setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));
        val = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val));
        setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val));
        val = config.keepalive_timeout;
        setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val));
    }

    FILE *f = fdopen(client_fd, "r+");
    if (f == NULL) {
        fprintf(stderr, "Socket console: fdopen failed: %s\n", strerror(errno));
        close(client_fd);
        vTaskDelete(NULL);
        return;
    }

    process_socket_commands(f);

    printf("Socket console: client disconnected %s:%d\n", client_ip_str,
            client_port);
    fclose(f);      // also closes client_fd
    vTaskDelete(NULL);
}

// Accepts connections and hands each one off to its own connection_task.
static void listener_task(void *arg)
{
    struct socket_console_config config = *(struct socket_console_config *)arg;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "Socket console: failed to create socket: %s\n",
                strerror(errno));
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(config.port);
    if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        fprintf(stderr, "Socket console: failed to bind socket: %s\n", strerror(errno));
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }
    if (listen(listen_fd, LISTEN_BACKLOG) < 0) {
        fprintf(stderr, "Socket console: failed to listen on socket: %s\n",
                strerror(errno));
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    printf("Socket console: listening on port %d.\n", config.port);

    struct sockaddr_in client_addr;
    while (1) {
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr,
                &addr_len);
        if (client_fd < 0) {
            fprintf(stderr, "Socket console: accept error: %s\n", strerror(errno));
            continue;
        }

        struct connection_args *args = malloc(sizeof(*args));
        if (args == NULL) {
            fprintf(stderr, "Socket console: out of memory accepting connection.\n");
            close(client_fd);
            continue;
        }
        args->client_fd = client_fd;
        args->config = config;
        inet_ntop(AF_INET, &client_addr.sin_addr, args->client_ip_str,
                sizeof(args->client_ip_str));
        args->client_port = ntohs(client_addr.sin_port);

        printf("Socket console: client connected %s:%d\n",
                args->client_ip_str, args->client_port);

        if (xTaskCreate(connection_task, "socket_console_conn",
                    CONNECTION_TASK_STACK_SIZE, args, CONNECTION_TASK_PRIORITY,
                    NULL) != pdPASS) {
            fprintf(stderr, "Socket console: failed to start connection task.\n");
            free(args);
            close(client_fd);
        }
    }
}

BaseType_t socket_console_start(const struct socket_console_config *config,
        const char *task_name, TaskHandle_t *handle)
{
    if (config == NULL)
        return pdFAIL;

    return xTaskCreate(listener_task,
            task_name ? task_name : "socket_console_task",
            LISTEN_TASK_STACK_SIZE, (void *) config,
            LISTEN_TASK_PRIORITY, handle);
}
