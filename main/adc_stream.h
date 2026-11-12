#ifndef ADC_STREAM_H
#define ADC_STREAM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum adc_stream_format {
    ADC_STREAM_FORMAT_TEXT,        // integer mV, newline-terminated
    ADC_STREAM_FORMAT_BINARY16,    // 16-bit little-endian signed mV
};

struct adc_stream_config {
    int port;
    int keepalive_timeout;         // seconds; 0 disables keepalive
    int gpio;                      // must resolve to an ADC1 channel
    int sample_rate_hz;            // output rate, after averaging
    int averaging_count;           // raw ADC samples averaged per output sample
    enum adc_stream_format format;
    int correction_curve_x1000000; // quadratic term, applied before gain/offset; 0 disables
    int correction_gain_x1000;     // 1000 == 1.000 (no gain correction)
    int correction_offset_mv;      // additive, may be negative
};

// Start the ADC streaming task. Only one instance may run at a time (ADC1
// is a single shared peripheral).
BaseType_t adc_stream_start(const struct adc_stream_config *config,
        const char *task_name, TaskHandle_t *handle);

void adc_stream_print_status(FILE *out);

// Internal GPIO-control query: includes the effective next-connection GPIO
// (NVS override or defaults) and the GPIO held by an existing ADC connection.
// Call while holding gpio_control_lock(), including during GPIO boot restore.
bool adc_stream_gpio_in_use(int gpio);

// Persist runtime-configurable settings (GPIO, output rate, averaging,
// format) to flash. Takes effect on the next TCP client connection, not
// the current one. TCP port and correction parameters remain CONFIG-only.
// With manual GPIO control enabled, rejects pins assigned to manual GPIO.
esp_err_t adc_stream_save_config(int gpio, int sample_rate_hz,
        int averaging_count, enum adc_stream_format format);

// Erase persisted settings; subsequent connections fall back to CONFIG
// defaults.
// With manual GPIO control enabled, checks the default pin for conflicts too.
esp_err_t adc_stream_clear_config(void);

#ifdef __cplusplus
}
#endif

#endif
