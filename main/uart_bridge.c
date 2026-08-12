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
 *     CONFIG_ESP_UART_BRIDGE_1_UART_NUM
 *     CONFIG_ESP_UART_BRIDGE_1_BAUD_RATE
 *     CONFIG_ESP_UART_BRIDGE_1_DATA_BITS
 *     CONFIG_ESP_UART_BRIDGE_1_PARITY
 *     CONFIG_ESP_UART_BRIDGE_1_STOP_BITS
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
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/unistd.h>
#include "tls_transport.h"
#include "uart_bridge.h"

#define BUFFER_SIZE         512
#define UART_BUFFER_SIZE    512

// Runtime-configurable UART line settings, persisted per UART number so
// that they survive a reboot. Namespace name is derived from the UART
// number to keep multiple simultaneous bridge instances independent.
#define UART_CONFIG_NVS_NAMESPACE_FMT   "uart_cfg%d"
#define UART_CONFIG_NVS_KEY_BAUD_RATE   "baud_rate"
#define UART_CONFIG_NVS_KEY_DATA_BITS   "data_bits"
#define UART_CONFIG_NVS_KEY_PARITY      "parity"
#define UART_CONFIG_NVS_KEY_STOP_BITS   "stop_bits"

// The TLS handshake (ECDHE + certificate parsing) runs inline in this
// task and needs substantially more stack than the rest of the task ever
// used before -- a plain 4096 overflowed and crashed the task in testing.
#if CONFIG_ESP_TLS_ENABLED
#define UART_BRIDGE_TASK_STACK_SIZE     8192
#else
#define UART_BRIDGE_TASK_STACK_SIZE     4096
#endif
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

static __thread struct uart_bridge_state *task_state;

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

esp_err_t uart_bridge_save_config(int uart_num, int baud_rate,
        uart_word_length_t data_bits, uart_parity_t parity,
        uart_stop_bits_t stop_bits)
{
    char ns[16];
    snprintf(ns, sizeof(ns), UART_CONFIG_NVS_NAMESPACE_FMT, uart_num);

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
        return err;

    err = nvs_set_i32(nvs_handle, UART_CONFIG_NVS_KEY_BAUD_RATE, baud_rate);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, UART_CONFIG_NVS_KEY_DATA_BITS,
                (uint8_t)data_bits);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, UART_CONFIG_NVS_KEY_PARITY,
                (uint8_t)parity);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, UART_CONFIG_NVS_KEY_STOP_BITS,
                (uint8_t)stop_bits);
    }
    if (err == ESP_OK)
        err = nvs_commit(nvs_handle);

    nvs_close(nvs_handle);
    return err;
}

esp_err_t uart_bridge_apply_live_config(int uart_num, int baud_rate,
        uart_word_length_t data_bits, uart_parity_t parity,
        uart_stop_bits_t stop_bits)
{
    uart_config_t uart_config = {
        .baud_rate  = baud_rate,
        .data_bits  = data_bits,
        .parity     = parity,
        .stop_bits  = stop_bits,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    return uart_param_config(uart_num, &uart_config);
}

esp_err_t uart_bridge_clear_config(int uart_num)
{
    char ns[16];
    snprintf(ns, sizeof(ns), UART_CONFIG_NVS_NAMESPACE_FMT, uart_num);

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
        return err;

    // Erase the entire namespace to clear all stored UART settings.
    err = nvs_erase_all(nvs_handle);
    if (err == ESP_OK)
        err = nvs_commit(nvs_handle);

    nvs_close(nvs_handle);
    return err;
}

// Read persisted UART line settings for uart_num. Returns true and fills in
// the output parameters if all settings were found in flash, false
// otherwise (caller should fall back to its own defaults).
static bool load_uart_config(int uart_num, int *baud_rate,
        uart_word_length_t *data_bits, uart_parity_t *parity,
        uart_stop_bits_t *stop_bits)
{
    char ns[16];
    snprintf(ns, sizeof(ns), UART_CONFIG_NVS_NAMESPACE_FMT, uart_num);

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
        return false;

    int32_t baud = 0;
    uint8_t dbits = 0;
    uint8_t par = 0;
    uint8_t sbits = 0;

    err = nvs_get_i32(nvs_handle, UART_CONFIG_NVS_KEY_BAUD_RATE, &baud);
    if (err == ESP_OK) {
        err = nvs_get_u8(nvs_handle, UART_CONFIG_NVS_KEY_DATA_BITS, &dbits);
    }
    if (err == ESP_OK) {
        err = nvs_get_u8(nvs_handle, UART_CONFIG_NVS_KEY_PARITY, &par);
    }
    if (err == ESP_OK) {
        err = nvs_get_u8(nvs_handle, UART_CONFIG_NVS_KEY_STOP_BITS, &sbits);
    }
    nvs_close(nvs_handle);
    if (err != ESP_OK)
        return false;

    *baud_rate = baud;
    *data_bits = (uart_word_length_t)dbits;
    *parity = (uart_parity_t)par;
    *stop_bits = (uart_stop_bits_t)sbits;
    return true;
}

static const char* parity_str(uart_parity_t parity)
{
    switch (parity) {
        case UART_PARITY_EVEN: return "E";
        case UART_PARITY_ODD:  return "O";
        default:               return "N";
    }
}

// Apply the effective UART line settings: settings stored in flash take
// precedence, falling back to the CONFIG-derived defaults in *config.
// Called at task startup and again on every new TCP client connection, so
// that a runtime settings change (via the 'uart' console command) takes
// effect for the next connection without requiring a reboot.
static void apply_uart_config(const struct uart_bridge_config *config)
{
    int baud_rate = config->baud_rate;
    uart_word_length_t data_bits = config->data_bits;
    uart_parity_t parity = config->parity;
    uart_stop_bits_t stop_bits = config->stop_bits;

    if (load_uart_config(config->uart_num, &baud_rate, &data_bits, &parity,
                &stop_bits)) {
        fprintf(stdout, "UART bridge %d: using UART%d settings from flash: "
                "%d-%d-%s-%d\n",
                task_state->config->instance, config->uart_num, baud_rate,
                data_bits == UART_DATA_7_BITS ? 7 : 8, parity_str(parity),
                stop_bits == UART_STOP_BITS_2 ? 2 : 1);
    } else {
        baud_rate = config->baud_rate;
        data_bits = config->data_bits;
        parity = config->parity;
        stop_bits = config->stop_bits;
        fprintf(stdout, "UART bridge %d: using UART%d settings from CONFIG: "
                "%d-%d-%s-%d\n",
                task_state->config->instance, config->uart_num, baud_rate,
                data_bits == UART_DATA_7_BITS ? 7 : 8, parity_str(parity),
                stop_bits == UART_STOP_BITS_2 ? 2 : 1);
    }

    uart_config_t uart_config = {
        .baud_rate  = baud_rate,
        .data_bits  = data_bits,
        .parity     = parity,
        .stop_bits  = stop_bits,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(config->uart_num, &uart_config));
}

void uart_bridge_print_status(FILE *out)
{
    struct {
        bool active;
        bool client_connected;
        int instance;
        int txd_pin;
        int rxd_pin;
        int port;
        int uart_num;
        int client_port;
        char client_ip_str[INET_ADDRSTRLEN];
        unsigned long count_rx;
        unsigned long count_tx;
        const struct uart_bridge_config *config;
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
        snapshot[i].instance = state->config->instance;
        snapshot[i].txd_pin = state->config->txd_pin;
        snapshot[i].rxd_pin = state->config->rxd_pin;
        snapshot[i].port = state->config->port;
        snapshot[i].uart_num = state->config->uart_num;
        snapshot[i].client_port = state->client_port;
        snapshot[i].count_rx = state->count_rx;
        snapshot[i].count_tx = state->count_tx;
        memcpy(snapshot[i].client_ip_str, state->client_ip_str,
                sizeof(snapshot[i].client_ip_str));
        snapshot[i].config = state->config;
    }
    portEXIT_CRITICAL(&task_states_mux);

    bool any = false;
    for (int i = 0; i < UART_BRIDGE_MAX_TASKS; i++) {
        if (!snapshot[i].active)
            continue;
        any = true;

        // Effective settings: same flash-with-CONFIG-fallback logic as
        // apply_uart_config(), computed fresh.
        int baud_rate = snapshot[i].config->baud_rate;
        uart_word_length_t data_bits_raw = snapshot[i].config->data_bits;
        uart_parity_t parity = snapshot[i].config->parity;
        uart_stop_bits_t stop_bits_raw = snapshot[i].config->stop_bits;
        load_uart_config(snapshot[i].uart_num, &baud_rate, &data_bits_raw,
                &parity, &stop_bits_raw);
        int data_bits = data_bits_raw == UART_DATA_7_BITS ? 7 : 8;
        int stop_bits = stop_bits_raw == UART_STOP_BITS_2 ? 2 : 1;
        fprintf(out, "UART bridge %d: UART%d, listening on port %d. "
                "%d-%d-%s-%d. GPIOs: TX=%d RX=%d.\n",
                snapshot[i].instance, snapshot[i].uart_num, snapshot[i].port,
                baud_rate, data_bits, parity_str(parity), stop_bits,
                snapshot[i].txd_pin, snapshot[i].rxd_pin);
        if (snapshot[i].client_connected) {
            fprintf(out, "UART bridge %d: connected to client '%s:%d'. "
                    "Bytes: TX=%lu RX=%lu.\n",
                    snapshot[i].instance, snapshot[i].client_ip_str,
                    snapshot[i].client_port, snapshot[i].count_tx,
                    snapshot[i].count_rx);
        }
    }
    if (!any)
        fprintf(out, "UART bridge: not running.\n");
}

static void uart_bridge_task(void* arg)
{
    struct uart_bridge_config config = *(struct uart_bridge_config*)arg;
    struct uart_bridge_state state = {0};
    state.config = &config;
    task_state = &state;
    int ret;

    int slot = reserve_resources(&state);
    if (slot < 0) {
        fprintf(stderr, "UART bridge %d: resource conflict on port or "
                "UART%d.\n", task_state->config->instance, config.uart_num);
        vTaskDelete(NULL);
        return;
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(listen_fd < 0) {
        fprintf(stderr, "UART bridge %d: Failed to create socket: %s\n",
                task_state->config->instance, strerror(errno));
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
        fprintf(stderr, "UART bridge %d: failed to bind socket: %s\n",
                task_state->config->instance, strerror(errno));
        release_resources(slot);
        vTaskDelete(NULL);
        return;
    }
    ret = listen(listen_fd, 1);
    if(ret < 0) {
        fprintf(stderr, "UART bridge %d: failed to listen on socket: %s\n",
                task_state->config->instance, strerror(errno));
        release_resources(slot);
        vTaskDelete(NULL);
        return;
    }

    // Set up UART.
    ret = uart_driver_install(config.uart_num,
            UART_BUFFER_SIZE, UART_BUFFER_SIZE, 0, NULL, 0);
    if(ret != ESP_OK) {
        fprintf(stderr, "UART bridge %d: UART%d driver installation "
                "failed\n", task_state->config->instance, config.uart_num);
        release_resources(slot);
        vTaskDelete(NULL);
        return;
    }
    uart_vfs_dev_register();

    apply_uart_config(&config);

    if(config.txd_pin != UART_PIN_NO_CHANGE ||
       config.rxd_pin != UART_PIN_NO_CHANGE) {

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

    fprintf(stdout, "UART bridge %d: UART%d, listening on port %d. GPIOs: "
            "TX=%d RX=%d\n", task_state->config->instance, config.uart_num,
            config.port, config.txd_pin, config.rxd_pin);

    // Select() loop blocks until activity on sockets or UART.
    struct sockaddr_in client_addr;
    transport_handle_t client_t = NULL;
    int uart_fd = -1;
    while (1) {
        fd_set read_fds;
        FD_ZERO(&read_fds);

        // Add listening socket and client socket to read_fds.
        FD_SET(listen_fd, &read_fds);
        if(client_t != NULL)
            FD_SET(transport_fd(client_t), &read_fds);
        if(uart_fd >= 0)
            FD_SET(uart_fd, &read_fds);
        int max_fd = MAX(listen_fd,
                MAX(client_t != NULL ? transport_fd(client_t) : -1, uart_fd));

        // Blocking call to select -- except if TLS already has a full
        // request buffered above the socket layer, in which case don't
        // block waiting for more raw bytes that may never come.
        struct timeval zero_tv = {0, 0};
        bool pending = client_t != NULL && transport_has_pending(client_t);
        int activity = select(max_fd+1, &read_fds, NULL, NULL,
                pending ? &zero_tv : NULL);
        if (activity < 0) {
            //ESP_LOGE(TAG, "select failed: errno %d", errno);
            fprintf(stderr, "UART bridge %d: select error: %s\n",
                    task_state->config->instance, strerror(errno));
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
                    fprintf(stderr, "UART bridge %d: accept error: %s\n",
                            task_state->config->instance, strerror(errno));
                }
            }
            else {
                if(client_t == NULL) {
                    // New client.
                    transport_handle_t new_t = transport_wrap(new_fd);
                    if (new_t == NULL) {
                        fprintf(stderr, "UART bridge %d: transport setup "
                                "failed, dropping client.\n",
                                task_state->config->instance);
                        continue;   // new_fd already closed
                    }
                    inet_ntop(AF_INET, &client_addr.sin_addr, state.client_ip_str,
                            sizeof(state.client_ip_str));
                    state.client_port = ntohs(client_addr.sin_port);
                    fprintf(stdout, "UART bridge %d: client connected %s:%d\n",
                            task_state->config->instance,
                            state.client_ip_str, state.client_port);

                    transport_set_keepalives(new_fd, config.keepalive_timeout);
                    transport_set_nonblocking(new_t);
                    client_t = new_t;

                    // Reapply UART settings now, in case they were changed
                    // via the console since the last connection (or since
                    // boot).
                    apply_uart_config(&config);

                    // Open UART.
                    uart_fd = open(uart_addr, O_RDWR);
                    if(uart_fd < 0) {
                        fprintf(stderr, "UART bridge %d: failed opening "
                                "UART%d: %s\n", task_state->config->instance,
                                config.uart_num, strerror(errno));
                        transport_close(client_t);
                        client_t = NULL;
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
                    fprintf(stderr, "UART bridge %d: dropping new connection. "
                            "Another client is already connected.\n",
                            task_state->config->instance);
                    close(new_fd);
                }
            }
        }

        // Handle client socket.
        if(client_t != NULL && (FD_ISSET(transport_fd(client_t), &read_fds) ||
                transport_has_pending(client_t))) {
            ret = transport_read(client_t, state.buffer, sizeof(state.buffer)-1);
            if(ret == 0 ||
              (ret < 0 && (errno == ECONNRESET || errno == ECONNABORTED ||
                           errno == ENOTCONN))) {
                // Client has disconnected.
                fprintf(stdout, "UART bridge %d: client disconnected.\n",
                        task_state->config->instance);
                transport_close(client_t);
                close(uart_fd);
                client_t = NULL;
                uart_fd = -1;
                state.count_rx = 0;
                state.count_tx = 0;
                state.client_connected = false;
                continue;       // restart select() loop
            }
            else if(ret < 0) {
                if(errno != EAGAIN && errno != EWOULDBLOCK)
                    fprintf(stderr, "UART bridge %d: socket read error: %s\n",
                            task_state->config->instance, strerror(errno));
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
                    fprintf(stderr, "UART bridge %d: UART%d read error: "
                            "%s\n", task_state->config->instance,
                            config.uart_num, strerror(errno));
            }
            else {
                state.count_rx += ret;
                if (transport_write_all(client_t, state.buffer, ret) != 0)
                    fprintf(stderr, "UART bridge %d: socket write error: %s\n",
                            task_state->config->instance, strerror(errno));
            }
        }
    }

    fprintf(stdout, "UART bridge %d: shutting down.\n",
            task_state->config->instance);
    state.client_connected = false;
    state.count_rx = 0;
    state.count_tx = 0;

    if(client_t != NULL)
        transport_close(client_t);
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

