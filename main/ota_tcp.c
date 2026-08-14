/*
 * SPDX-FileCopyrightText: Brian Kuschak <bkuschak@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Raw TCP firmware update (OTA).
 *
 * Host side needs no custom tooling beyond ncat. A client just streams a
 * firmware image and closes the connection:
 *
 *   ncat <esp32-host> <ota-port> < firmware.bin
 *
 * A script has been provided to automate OTA updates for both plaintext and
 * TLS modes:
 *
 * Plaintext:
 *   HOST=192.168.1.42 ./ota_push.sh build/build_esp32s3_devkit_c1/firmware.bin
 *
 * TLS (with CONFIG_ESP_TLS_ENABLED=y):
 *   HOST=192.168.1.42 CERT_DIR=host/certs ./ota_push.sh \
 *       build/build_esp32s3_devkit_c1/firmware.bin
 *
 * Bytes are streamed straight into flash via esp_ota_write() as they arrive.
 * No hash/checksum of our own: esp_ota_end() already re-reads the image back
 * from flash and verifies ESP-IDF's own embedded SHA-256 (plus chip ID, chip
 * revision, and image structure) before we call esp_ota_set_boot_partition().
 * Only one client at a time.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/unistd.h>
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_tcp.h"
#include "tls_transport.h"

// TLS handshake runs inline and needs extra stack, same as adc_stream.c.
#if CONFIG_ESP_TLS_ENABLED
#define OTA_TCP_TASK_STACK_SIZE     8192
#else
#define OTA_TCP_TASK_STACK_SIZE     4096
#endif
#define OTA_TCP_TASK_PRIORITY       5

#define OTA_TCP_READ_CHUNK          4096    // matches uart_bridge.c's convention

// Let the final response actually flush before esp_restart() tears the
// connection down.
#define OTA_TCP_RESTART_DELAY_MS    500

// Writes "FAIL: <name>\n" for err, clamped to msg's size.
static void send_fail(transport_handle_t t, esp_err_t err)
{
    char msg[64];
    int len = snprintf(msg, sizeof(msg), "FAIL: %s\n", esp_err_to_name(err));
    if (len > (int)sizeof(msg) - 1)
        len = sizeof(msg) - 1;
    transport_write_all(t, msg, len);
}

// Streams the connection into ota_handle until EOF. Caller still owns
// ota_handle either way -- never calls esp_ota_end()/esp_ota_abort().
// Image validity is esp_ota_end()'s job (see file header).
static esp_err_t receive_and_write(transport_handle_t t,
        esp_ota_handle_t ota_handle)
{
    uint8_t *buf = malloc(OTA_TCP_READ_CHUNK);
    if (buf == NULL) {
        fprintf(stderr, "OTA: out of memory allocating read buffer.\n");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = ESP_OK;
    size_t total_len = 0;

    while (1) {
        ssize_t n = transport_read(t, buf, OTA_TCP_READ_CHUNK);

        // Treat ECONNRESET as EOF too: pipe-based clients don't always send
        // a clean close_notify. esp_ota_end() still catches a truncated
        // image.
        if (n == 0 || (n < 0 && errno == ECONNRESET))
            break;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            fprintf(stderr, "OTA: read error: %s\n", strerror(errno));
            result = ESP_FAIL;
            goto done;
        }

        esp_err_t err = esp_ota_write(ota_handle, buf, (size_t)n);
        if (err != ESP_OK) {
            fprintf(stderr, "OTA: esp_ota_write failed: %s\n",
                    esp_err_to_name(err));
            result = err;
            goto done;
        }
        total_len += (size_t)n;
    }

    fprintf(stdout, "OTA: %" PRIu32 " bytes received.\n", (uint32_t)total_len);

done:
    free(buf);
    return result;
}

static void ota_tcp_task(void *arg)
{
    struct ota_tcp_config config = *(struct ota_tcp_config *)arg;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "OTA: failed to create socket: %s\n",
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
    if (bind(listen_fd, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0) {
        fprintf(stderr, "OTA: failed to bind socket: %s\n", strerror(errno));
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }
    if (listen(listen_fd, 1) < 0) {
        fprintf(stderr, "OTA: failed to listen on socket: %s\n",
                strerror(errno));
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    fprintf(stdout, "OTA: listening on port %d.\n", config.port);

    struct sockaddr_in client_addr;
    while (1) {
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr,
                &addr_len);
        if (client_fd < 0) {
            fprintf(stderr, "OTA: accept error: %s\n", strerror(errno));
            continue;
        }
        transport_handle_t t = transport_wrap(client_fd);
        if (t == NULL) {
            fprintf(stderr, "OTA: transport setup failed, dropping "
                    "client.\n");
            continue;   // client_fd already closed
        }
        transport_set_keepalives(client_fd, config.keepalive_timeout);

        char client_ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip_str,
                sizeof(client_ip_str));
        fprintf(stdout, "OTA: client connected %s:%d\n", client_ip_str,
                ntohs(client_addr.sin_port));

        const esp_partition_t *target =
            esp_ota_get_next_update_partition(NULL);
        if (target == NULL) {
            fprintf(stderr, "OTA: no OTA partition available.\n");
            transport_write_all(t, "FAIL: no OTA partition\n", 24);
            transport_close(t);
            continue;
        }

        esp_ota_handle_t ota_handle;
        esp_err_t err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &ota_handle);
        if (err != ESP_OK) {
            fprintf(stderr, "OTA: esp_ota_begin failed: %s\n",
                    esp_err_to_name(err));
            send_fail(t, err);
            transport_close(t);
            continue;
        }

        err = receive_and_write(t, ota_handle);
        if (err == ESP_OK) {
            err = esp_ota_end(ota_handle);
            if (err != ESP_OK) {
                fprintf(stderr, "OTA: esp_ota_end failed: %s\n",
                        esp_err_to_name(err));
            }
        } else {
            esp_ota_abort(ota_handle);
        }

        if (err == ESP_OK) {
            err = esp_ota_set_boot_partition(target);
            if (err != ESP_OK) {
                fprintf(stderr, "OTA: esp_ota_set_boot_partition failed: "
                        "%s\n", esp_err_to_name(err));
            }
        }

        if (err == ESP_OK) {
            fprintf(stdout, "OTA: update succeeded, rebooting into '%s'.\n",
                    target->label);
            transport_write_all(t, "OK\n", 3);
            transport_close(t);
            vTaskDelay(pdMS_TO_TICKS(OTA_TCP_RESTART_DELAY_MS));
            esp_restart();
            // Not reached.
        }

        send_fail(t, err);
        transport_close(t);
        fprintf(stdout, "OTA: update failed, still running current "
                "firmware.\n");
    }

    close(listen_fd);
    vTaskDelete(NULL);
}

BaseType_t ota_tcp_start(const struct ota_tcp_config *config,
        const char *task_name, TaskHandle_t *handle)
{
    if (config == NULL)
        return pdFAIL;

    return xTaskCreate(ota_tcp_task, task_name ? task_name : "ota_tcp_task",
            OTA_TCP_TASK_STACK_SIZE, (void *)config,
            OTA_TCP_TASK_PRIORITY, handle);
}
