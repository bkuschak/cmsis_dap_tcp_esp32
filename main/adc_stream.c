/*
 * SPDX-FileCopyrightText: Brian Kuschak <bkuschak@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 *
 * ADC measurement streaming over TCP/IP.
 *
 * Streams averaged, calibrated ADC1 measurements (in mV) to one TCP client via
 * the ADC continuous (DMA) driver. Sampling starts on connect, stops on
 * disconnect. ADC1 only -- ADC2 conflicts with WiFi. Output range is 0 to 3300
 * mV. The ADC is quite noisy, even with averaging, so there appears to be no
 * benefit to output a resolution finer than 1 mV.
 *
 * Kconfig options: see Kconfig.projbuild (CONFIG_ESP_ADC_STREAM_*). GPIO,
 * rate, averaging, and format can also be set at runtime via the 'adc'
 * console command (persisted to flash, takes precedence over CONFIG,
 * applied starting with the next connection).
 *
 * Only one instance may run -- ADC1 is a single shared peripheral.
 */

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_continuous.h"
#include "esp_attr.h"
#include "errno.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "netdb.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "soc/soc_caps.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <sys/unistd.h>
#include "adc_stream.h"

#define ADC_STREAM_TASK_STACK_SIZE     4096
#define ADC_STREAM_TASK_PRIORITY       5
#define ADC_STREAM_MAX_TASKS           1   // ADC1 is a single shared peripheral

// Fixed namespace, not keyed by port like uart_bridge -- only one ADC
// stream instance can ever run (ADC1 is a single shared peripheral).
#define ADC_STREAM_CONFIG_NVS_NAMESPACE    "adc_cfg"
#define ADC_STREAM_CONFIG_NVS_KEY_GPIO     "gpio"
#define ADC_STREAM_CONFIG_NVS_KEY_SPS      "sps"
#define ADC_STREAM_CONFIG_NVS_KEY_AVG      "avg"
#define ADC_STREAM_CONFIG_NVS_KEY_FORMAT   "fmt"

// Frame sizing: floor avoids an interrupt per sample at low averaging;
// ceiling bounds per-connection heap use.
#define ADC_STREAM_MIN_FRAME_SAMPLES   64
#define ADC_STREAM_MAX_FRAME_SAMPLES   1024

// Output width for the oversampling shift below, and the binary16 wire
// width -- averaging beyond this only reduces noise, can't add resolution.
#define ADC_STREAM_OUTPUT_BITS         16

// Uncalibrated fallback full-scale (ADC_ATTEN_DB_12, ~3.3V) if eFuse
// calibration isn't burnt.
#define ADC_STREAM_FALLBACK_FULL_SCALE_MV  3300

// Per-connection state, kept on adc_stream_task()'s stack frame.
struct adc_stream_state {
    const struct adc_stream_config *config;    // CONFIG baseline; never mutated
    // Effective settings, resolved by apply_adc_stream_config(): flash
    // override if present, else config->*.
    int active_gpio;
    int active_sample_rate_hz;
    int active_averaging_count;
    enum adc_stream_format active_format;
    char client_ip_str[INET_ADDRSTRLEN];
    int client_port;
    bool client_connected;
    unsigned long count_tx;
    volatile uint32_t overflow_count;   // written by ISR, read by task
    uint32_t railed_count;              // raw samples seen at 0 or full-scale
};

// Registry of running tasks, for conflict detection and status reporting.
static struct adc_stream_state *task_states[ADC_STREAM_MAX_TASKS];
static portMUX_TYPE task_states_mux = portMUX_INITIALIZER_UNLOCKED;

static int reserve_resources(struct adc_stream_state *state)
{
    int slot = -1;

    portENTER_CRITICAL(&task_states_mux);
    for (int i = 0; i < ADC_STREAM_MAX_TASKS; i++) {
        if (task_states[i] == NULL) {
            if (slot < 0)
                slot = i;
        } else if (task_states[i]->config->port == state->config->port) {
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

static const char *format_str(enum adc_stream_format format)
{
    return format == ADC_STREAM_FORMAT_BINARY16 ? "binary16" : "text";
}

esp_err_t adc_stream_save_config(int gpio, int sample_rate_hz,
        int averaging_count, enum adc_stream_format format)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(ADC_STREAM_CONFIG_NVS_NAMESPACE, NVS_READWRITE,
            &nvs_handle);
    if (err != ESP_OK)
        return err;

    err = nvs_set_i32(nvs_handle, ADC_STREAM_CONFIG_NVS_KEY_GPIO, gpio);
    if (err == ESP_OK) {
        err = nvs_set_i32(nvs_handle, ADC_STREAM_CONFIG_NVS_KEY_SPS,
                sample_rate_hz);
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(nvs_handle, ADC_STREAM_CONFIG_NVS_KEY_AVG,
                averaging_count);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, ADC_STREAM_CONFIG_NVS_KEY_FORMAT,
                (uint8_t)format);
    }
    if (err == ESP_OK)
        err = nvs_commit(nvs_handle);

    nvs_close(nvs_handle);
    return err;
}

esp_err_t adc_stream_clear_config(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(ADC_STREAM_CONFIG_NVS_NAMESPACE, NVS_READWRITE,
            &nvs_handle);
    if (err != ESP_OK)
        return err;

    err = nvs_erase_all(nvs_handle);
    if (err == ESP_OK)
        err = nvs_commit(nvs_handle);

    nvs_close(nvs_handle);
    return err;
}

// Returns true and fills the outputs if all settings were found in flash.
static bool load_adc_stream_config(int *gpio, int *sample_rate_hz,
        int *averaging_count, enum adc_stream_format *format)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(ADC_STREAM_CONFIG_NVS_NAMESPACE, NVS_READONLY,
            &nvs_handle);
    if (err != ESP_OK)
        return false;

    int32_t g = 0, sps = 0, avg = 0;
    uint8_t fmt = 0;

    err = nvs_get_i32(nvs_handle, ADC_STREAM_CONFIG_NVS_KEY_GPIO, &g);
    if (err == ESP_OK)
        err = nvs_get_i32(nvs_handle, ADC_STREAM_CONFIG_NVS_KEY_SPS, &sps);
    if (err == ESP_OK)
        err = nvs_get_i32(nvs_handle, ADC_STREAM_CONFIG_NVS_KEY_AVG, &avg);
    if (err == ESP_OK)
        err = nvs_get_u8(nvs_handle, ADC_STREAM_CONFIG_NVS_KEY_FORMAT, &fmt);
    nvs_close(nvs_handle);
    if (err != ESP_OK)
        return false;

    *gpio = g;
    *sample_rate_hz = sps;
    *averaging_count = avg;
    *format = (enum adc_stream_format)fmt;
    return true;
}

// Resolves state->active_* -- flash overrides *config if present. Must not
// modify *config (needs to stay pristine after a console 'adc' clear).
// Called per-connection so runtime changes apply without a reboot.
static void apply_adc_stream_config(const struct adc_stream_config *config,
        struct adc_stream_state *state)
{
    int gpio = config->gpio;
    int sample_rate_hz = config->sample_rate_hz;
    int averaging_count = config->averaging_count;
    enum adc_stream_format format = config->format;

    if (load_adc_stream_config(&gpio, &sample_rate_hz, &averaging_count,
                &format)) {
        fprintf(stdout, "ADC stream: using settings from flash: GPIO=%d, "
                "%d Hz, averaging=%d, format=%s.\n", gpio, sample_rate_hz,
                averaging_count, format_str(format));
    } else {
        gpio = config->gpio;
        sample_rate_hz = config->sample_rate_hz;
        averaging_count = config->averaging_count;
        format = config->format;
        fprintf(stdout, "ADC stream: using settings from CONFIG: GPIO=%d, "
                "%d Hz, averaging=%d, format=%s.\n", gpio, sample_rate_hz,
                averaging_count, format_str(format));
    }

    state->active_gpio = gpio;
    state->active_sample_rate_hz = sample_rate_hz;
    state->active_averaging_count = averaging_count;
    state->active_format = format;
}

void adc_stream_print_status(void)
{
    struct {
        bool active;
        bool client_connected;
        int gpio;
        int sample_rate_hz;
        int averaging_count;
        enum adc_stream_format format;
        int port;
        int client_port;
        char client_ip_str[INET_ADDRSTRLEN];
        unsigned long count_tx;
        uint32_t overflow_count;
        uint32_t railed_count;
    } snapshot[ADC_STREAM_MAX_TASKS] = {0};

    portENTER_CRITICAL(&task_states_mux);
    for (int i = 0; i < ADC_STREAM_MAX_TASKS; i++) {
        struct adc_stream_state *state = task_states[i];
        if (state == NULL)
            continue;
        snapshot[i].active = true;
        snapshot[i].client_connected = state->client_connected;
        snapshot[i].gpio = state->active_gpio;
        snapshot[i].sample_rate_hz = state->active_sample_rate_hz;
        snapshot[i].averaging_count = state->active_averaging_count;
        snapshot[i].format = state->active_format;
        snapshot[i].port = state->config->port;
        snapshot[i].client_port = state->client_port;
        snapshot[i].count_tx = state->count_tx;
        snapshot[i].overflow_count = state->overflow_count;
        snapshot[i].railed_count = state->railed_count;
        memcpy(snapshot[i].client_ip_str, state->client_ip_str,
                sizeof(snapshot[i].client_ip_str));
    }
    portEXIT_CRITICAL(&task_states_mux);

    bool any = false;
    for (int i = 0; i < ADC_STREAM_MAX_TASKS; i++) {
        if (!snapshot[i].active)
            continue;
        any = true;
        if (snapshot[i].client_connected) {
            fprintf(stdout, "ADC stream: port %d connected to '%s:%d'. "
                    "GPIO=%d, %d Hz, averaging=%d, format=%s. Bytes TX=%lu. "
                    "Overflows=%" PRIu32 ". Railed samples=%" PRIu32 ".\n",
                    snapshot[i].port, snapshot[i].client_ip_str,
                    snapshot[i].client_port, snapshot[i].gpio,
                    snapshot[i].sample_rate_hz, snapshot[i].averaging_count,
                    format_str(snapshot[i].format), snapshot[i].count_tx,
                    snapshot[i].overflow_count, snapshot[i].railed_count);
        } else {
            fprintf(stdout, "ADC stream: listening on port %d. GPIO=%d, "
                    "%d Hz, averaging=%d, format=%s.\n",
                    snapshot[i].port, snapshot[i].gpio,
                    snapshot[i].sample_rate_hz, snapshot[i].averaging_count,
                    format_str(snapshot[i].format));
        }
    }
    if (!any)
        fprintf(stdout, "ADC stream: not running.\n");
}

// ISR context, must be IRAM-resident. Diagnostic only -- flush_pool=1
// below already self-heals by dropping the oldest frame.
static bool IRAM_ATTR adc_stream_pool_ovf_cb(adc_continuous_handle_t handle,
        const adc_continuous_evt_data_t *edata, void *user_data)
{
    volatile uint32_t *overflow_count = (volatile uint32_t *)user_data;
    (*overflow_count)++;
    return false;
}

// Returns false (and logs why) if gpio isn't a valid ADC1 pin on this chip.
static bool resolve_adc1_channel(int gpio, adc_channel_t *out_channel)
{
    adc_unit_t unit;
    adc_channel_t channel;

    esp_err_t err = adc_continuous_io_to_channel(gpio, &unit, &channel);
    if (err != ESP_OK) {
        fprintf(stderr, "ADC stream: GPIO%d is not a valid ADC input on "
                "this chip.\n", gpio);
        return false;
    }
    if (unit != ADC_UNIT_1) {
        fprintf(stderr, "ADC stream: GPIO%d maps to ADC2, which conflicts "
                "with the WiFi driver. Choose an ADC1 GPIO instead.\n",
                gpio);
        return false;
    }

    *out_channel = channel;
    return true;
}

static void adc_stream_task(void* arg)
{
    struct adc_stream_config config = *(struct adc_stream_config*)arg;
    struct adc_stream_state state = {0};
    state.config = &config;
    int ret;

    int slot = reserve_resources(&state);
    if (slot < 0) {
        fprintf(stderr, "ADC stream: resource conflict on port %d, or "
                "another instance is already running.\n", config.port);
        vTaskDelete(NULL);
        return;
    }

    // Just so state.active_* has something to report before a client
    // connects -- real validation happens per-connection below.
    apply_adc_stream_config(&config, &state);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "ADC stream: failed to create socket: %s\n",
                strerror(errno));
        release_resources(slot);
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(config.port);
    ret = bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        fprintf(stderr, "ADC stream: failed to bind socket: %s\n",
                strerror(errno));
        close(listen_fd);
        release_resources(slot);
        vTaskDelete(NULL);
        return;
    }
    ret = listen(listen_fd, 1);
    if (ret < 0) {
        fprintf(stderr, "ADC stream: failed to listen on socket: %s\n",
                strerror(errno));
        close(listen_fd);
        release_resources(slot);
        vTaskDelete(NULL);
        return;
    }

    fprintf(stdout, "ADC stream: listening on port %d.\n", config.port);

    struct sockaddr_in client_addr;
    while (1) {
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr,
                &addr_len);
        if (client_fd < 0) {
            fprintf(stderr, "ADC stream: accept error: %s\n", strerror(errno));
            continue;
        }
        fcntl(client_fd, F_SETFL, O_NONBLOCK);

        inet_ntop(AF_INET, &client_addr.sin_addr, state.client_ip_str,
                sizeof(state.client_ip_str));
        state.client_port = ntohs(client_addr.sin_port);
        fprintf(stdout, "ADC stream: client connected %s:%d\n",
                state.client_ip_str, state.client_port);

        if (config.keepalive_timeout > 0) {
            int val = 1;
            setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));
            val = 1;
            setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val));
            setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val));
            val = config.keepalive_timeout;
            setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val));
        }

        // Reload in case settings changed via console since the last
        // connection. A bad value here only refuses this connection.
        apply_adc_stream_config(&config, &state);

        adc_channel_t channel;
        if (!resolve_adc1_channel(state.active_gpio, &channel)) {
            close(client_fd);
            continue;
        }

        if (state.active_averaging_count < 1 ||
                (state.active_averaging_count &
                 (state.active_averaging_count - 1)) != 0) {
            fprintf(stderr, "ADC stream: averaging (%d) must be a power of "
                    "2 (the accumulate-and-shift oversampling below relies "
                    "on it).\n", state.active_averaging_count);
            close(client_fd);
            continue;
        }

        uint32_t raw_sample_freq_hz = (uint32_t)state.active_sample_rate_hz *
            (uint32_t)state.active_averaging_count;
        if (raw_sample_freq_hz < SOC_ADC_SAMPLE_FREQ_THRES_LOW ||
                raw_sample_freq_hz > SOC_ADC_SAMPLE_FREQ_THRES_HIGH) {
            fprintf(stderr, "ADC stream: output rate (%d) x averaging (%d) "
                    "= %" PRIu32 " Hz raw ADC rate, outside the supported "
                    "range (%d - %d Hz).\n",
                    state.active_sample_rate_hz, state.active_averaging_count,
                    raw_sample_freq_hz, SOC_ADC_SAMPLE_FREQ_THRES_LOW,
                    SOC_ADC_SAMPLE_FREQ_THRES_HIGH);
            close(client_fd);
            continue;
        }

        uint32_t frame_samples = state.active_averaging_count;
        if (frame_samples < ADC_STREAM_MIN_FRAME_SAMPLES)
            frame_samples = ADC_STREAM_MIN_FRAME_SAMPLES;
        if (frame_samples > ADC_STREAM_MAX_FRAME_SAMPLES)
            frame_samples = ADC_STREAM_MAX_FRAME_SAMPLES;
        uint32_t conv_frame_size = frame_samples * SOC_ADC_DIGI_RESULT_BYTES;

        state.count_tx = 0;
        state.overflow_count = 0;
        state.railed_count = 0;
        state.client_connected = true;

        // --- Start ADC sampling for this connection ---
        uint8_t *raw_buf = malloc(conv_frame_size);
        adc_continuous_data_t *parsed = malloc(
                frame_samples * sizeof(adc_continuous_data_t));
        adc_continuous_handle_t adc_handle = NULL;
        adc_cali_handle_t cali_handle = NULL;
        bool started = false;

        if (raw_buf == NULL || parsed == NULL) {
            fprintf(stderr, "ADC stream: out of memory allocating buffers.\n");
            goto disconnect;
        }

        adc_continuous_handle_cfg_t handle_cfg = {
            .max_store_buf_size = conv_frame_size * 4,
            .conv_frame_size = conv_frame_size,
            .flags.flush_pool = 1,  // self-heal on overflow; on_pool_ovf just counts it
        };
        if (adc_continuous_new_handle(&handle_cfg, &adc_handle) != ESP_OK) {
            fprintf(stderr, "ADC stream: failed to create ADC handle.\n");
            goto disconnect;
        }

        adc_digi_pattern_config_t pattern = {
            .atten = ADC_ATTEN_DB_12,      // ~3.3V full scale
            .channel = channel,
            .unit = ADC_UNIT_1,
            .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,    // 12-bit native
        };
        adc_continuous_config_t dig_cfg = {
            .pattern_num = 1,
            .adc_pattern = &pattern,
            .sample_freq_hz = raw_sample_freq_hz,
            .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        };
        if (adc_continuous_config(adc_handle, &dig_cfg) != ESP_OK) {
            fprintf(stderr, "ADC stream: failed to configure ADC.\n");
            goto disconnect;
        }

        adc_continuous_evt_cbs_t cbs = {
            .on_pool_ovf = adc_stream_pool_ovf_cb,
        };
        if (adc_continuous_register_event_callbacks(adc_handle, &cbs,
                    (void *)&state.overflow_count) != ESP_OK) {
            fprintf(stderr, "ADC stream: failed to register overflow "
                    "callback; overflow detection disabled for this "
                    "connection.\n");
        }

        // Best-effort eFuse curve-fitting calibration; falls back to a
        // linear estimate if unsupported.
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = ADC_UNIT_1,
            .chan = channel,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = SOC_ADC_DIGI_MAX_BITWIDTH,
        };
        esp_err_t cali_err = adc_cali_create_scheme_curve_fitting(&cali_cfg,
                &cali_handle);
        if (cali_err != ESP_OK) {
            fprintf(stderr, "ADC stream: calibration unavailable (%s), "
                    "using uncalibrated approximation.\n",
                    esp_err_to_name(cali_err));
            cali_handle = NULL;
        }

        if (adc_continuous_start(adc_handle) != ESP_OK) {
            fprintf(stderr, "ADC stream: failed to start ADC.\n");
            goto disconnect;
        }
        started = true;

        // --- Streaming loop ---
        // averaging_count is a power of 2, so the raw sum carries shift_n
        // extra sub-LSB bits; shift (never divide) below to preserve them.
        int shift_n = __builtin_ctz((unsigned)state.active_averaging_count);
        int frac_bits = ADC_STREAM_OUTPUT_BITS - SOC_ADC_DIGI_MAX_BITWIDTH;
        int shift = shift_n - frac_bits;

        uint64_t sum = 0;
        uint32_t sum_count = 0;
        uint32_t last_overflow_seen = 0;
        while (1) {
            // Check for client disconnect / error without blocking.
            char discard[16];
            ret = recv(client_fd, discard, sizeof(discard), MSG_DONTWAIT);
            if (ret == 0 ||
                    (ret < 0 && (errno == ECONNRESET || errno == ENOTCONN ||
                                 errno == ECONNABORTED))) {
                fprintf(stdout, "ADC stream: client disconnected.\n");
                break;
            }

            uint32_t overflow_now = state.overflow_count;
            if (overflow_now != last_overflow_seen) {
                fprintf(stderr, "ADC stream: DMA pool overflow (total %"
                        PRIu32 "); oldest buffered data was dropped.\n",
                        overflow_now);
                last_overflow_seen = overflow_now;
            }

            uint32_t out_len = 0;
            esp_err_t read_ret = adc_continuous_read(adc_handle, raw_buf,
                    conv_frame_size, &out_len, 100 /* ms */);
            if (read_ret == ESP_ERR_TIMEOUT) {
                continue;
            } else if (read_ret != ESP_OK) {
                fprintf(stderr, "ADC stream: ADC read error: %s\n",
                        esp_err_to_name(read_ret));
                break;
            }

            uint32_t num_parsed = 0;
            if (adc_continuous_parse_data(adc_handle, raw_buf, out_len,
                        parsed, &num_parsed) != ESP_OK) {
                continue;
            }

            for (uint32_t i = 0; i < num_parsed; i++) {
                if (!parsed[i].valid)
                    continue;
                uint32_t raw = parsed[i].raw_data;
                if (raw == 0 || raw >= ((1u << SOC_ADC_DIGI_MAX_BITWIDTH) - 1))
                    state.railed_count++;

                sum += raw;
                sum_count++;
                if (sum_count < (uint32_t)state.active_averaging_count)
                    continue;

                // Shift (not divide) to keep sub-LSB precision.
                uint32_t raw_scaled = (shift >= 0) ?
                    (uint32_t)(sum >> shift) : (uint32_t)(sum << (-shift));
                sum = 0;
                sum_count = 0;

                int mv;
                if (cali_handle != NULL) {
                    // adc_cali_raw_to_voltage() only takes an integer raw
                    // code; interpolate between floor/ceil codes to keep
                    // the sub-LSB fraction.
                    int raw_floor = (int)(raw_scaled >> frac_bits);
                    int frac = (int)(raw_scaled & ((1u << frac_bits) - 1));
                    int raw_ceil = raw_floor + 1;
                    if (raw_ceil > ((1 << SOC_ADC_DIGI_MAX_BITWIDTH) - 1))
                        raw_ceil = raw_floor;
                    int mv_floor, mv_ceil;
                    adc_cali_raw_to_voltage(cali_handle, raw_floor, &mv_floor);
                    adc_cali_raw_to_voltage(cali_handle, raw_ceil, &mv_ceil);
                    mv = mv_floor +
                        (frac * (mv_ceil - mv_floor)) / (1 << frac_bits);
                } else {
                    // Already linear -- use raw_scaled directly, no
                    // rounding needed.
                    mv = (int)(((uint64_t)raw_scaled *
                                ADC_STREAM_FALLBACK_FULL_SCALE_MV) /
                            (((1u << SOC_ADC_DIGI_MAX_BITWIDTH) - 1u) <<
                             frac_bits));
                }

                // Curvature correction, then linear gain/offset trim
                // (CONFIG_ESP_ADC_STREAM_CORRECTION_*).
                int64_t curve_term = (int64_t)config.correction_curve_x1000000
                    * mv * mv / 1000000;
                mv = mv + (int)curve_term;
                mv = (mv * config.correction_gain_x1000) / 1000 +
                    config.correction_offset_mv;

                if (state.active_format == ADC_STREAM_FORMAT_BINARY16) {
                    if (mv > INT16_MAX)
                        mv = INT16_MAX;
                    if (mv < INT16_MIN)
                        mv = INT16_MIN;
                    uint8_t out[2] = {
                        (uint8_t)(((int16_t)mv) & 0xff),
                        (uint8_t)(((int16_t)mv) >> 8),
                    };
                    ret = send(client_fd, out, sizeof(out), 0);
                    if (ret > 0)
                        state.count_tx += ret;
                } else {
                    char out[16];
                    int len = snprintf(out, sizeof(out), "%d\n", mv);
                    ret = send(client_fd, out, len, 0);
                    if (ret > 0)
                        state.count_tx += ret;
                }
                if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    fprintf(stdout, "ADC stream: client disconnected "
                            "(send error).\n");
                    goto stream_done;
                }
            }
        }
stream_done:

disconnect:
        if (started)
            adc_continuous_stop(adc_handle);
        if (cali_handle != NULL)
            adc_cali_delete_scheme_curve_fitting(cali_handle);
        if (adc_handle != NULL)
            adc_continuous_deinit(adc_handle);
        free(raw_buf);
        free(parsed);

        close(client_fd);
        state.client_connected = false;
        state.count_tx = 0;
    }

    close(listen_fd);
    release_resources(slot);
    vTaskDelete(NULL);
}

BaseType_t adc_stream_start(const struct adc_stream_config *config,
        const char *task_name, TaskHandle_t *handle)
{
    if (config == NULL)
        return pdFAIL;

    return xTaskCreate(adc_stream_task,
            task_name ? task_name : "adc_stream_task",
            ADC_STREAM_TASK_STACK_SIZE, (void *) config,
            ADC_STREAM_TASK_PRIORITY, handle);
}
