/*
 * SPDX-FileCopyrightText: Brian Kuschak <bkuschak@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Drive the WS2812 RGB LED(s) used on some boards. Supports one LED per
 * distinct GPIO, so multiple CMSIS-DAP TCP instances can each drive their
 * own status LED concurrently, if they exist.
 */

#include <stdio.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip.h"
#include "sdkconfig.h"
#include "ws2812_led.h"

// Bounds the number of independently-driven WS2812 status LEDs. Matches
// CMSIS_DAP_TCP_MAX_TASKS -- at most one LED per CMSIS-DAP TCP instance.
#define WS2812_LED_MAX_LEDS   4

enum led_status {
    WS2812_LED_UNUSED = 0,
    WS2812_LED_ACTIVE,
    WS2812_LED_FAILED,   // init failed (e.g. no RMT channel available); don't retry
};

struct ws2812_led {
    int gpio;
    enum led_status status;
    led_strip_handle_t handle;
};

static struct ws2812_led leds[WS2812_LED_MAX_LEDS];

// Guards both the lazy creation of 'lock' below and all access to 'leds'.
static portMUX_TYPE lock_init_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t lock;

static void ensure_lock(void)
{
    if (lock != NULL)
        return;

    portENTER_CRITICAL(&lock_init_mux);
    if (lock == NULL)
        lock = xSemaphoreCreateMutex();
    portEXIT_CRITICAL(&lock_init_mux);
}

static esp_err_t init_led(int gpio, led_strip_handle_t *out)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = gpio,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 100ns resolution
#ifdef CONFIG_IDF_TARGET_ESP32C6
        .mem_block_symbols = 64, // Required for C6 architecture
#else
        .mem_block_symbols = 0,
#endif
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, out);
    if (err == ESP_OK)
        led_strip_clear(*out);
    return err;
}

// Find the LED driving 'gpio', creating it on first use. Returns NULL if
// 'gpio' has no usable LED (creation failed, or no slots remain) -- the
// caller then just skips driving that LED instead of failing the instance.
static struct ws2812_led *get_led(int gpio)
{
    struct ws2812_led *unused = NULL;

    for (int i = 0; i < WS2812_LED_MAX_LEDS; i++) {
        if (leds[i].status != WS2812_LED_UNUSED && leds[i].gpio == gpio)
            return leds[i].status == WS2812_LED_ACTIVE ? &leds[i] : NULL;
        if (leds[i].status == WS2812_LED_UNUSED && unused == NULL)
            unused = &leds[i];
    }

    if (unused == NULL) {
        fprintf(stderr, "ws2812_led: no free LED slots for GPIO %d, "
                "disabling this LED.\n", gpio);
        return NULL;
    }

    esp_err_t err = init_led(gpio, &unused->handle);
    if (err != ESP_OK) {
        fprintf(stderr, "ws2812_led: failed to init WS2812 driver on GPIO "
                "%d (%s), disabling this LED.\n", gpio, esp_err_to_name(err));
        unused->gpio = gpio;
        unused->status = WS2812_LED_FAILED;
        return NULL;
    }

    unused->gpio = gpio;
    unused->status = WS2812_LED_ACTIVE;
    return unused;
}

void set_rgb_led(int gpio, uint8_t r, uint8_t g, uint8_t b)
{
    if (gpio < 0)
        return;

    ensure_lock();
    xSemaphoreTake(lock, portMAX_DELAY);

    struct ws2812_led *led = get_led(gpio);
    if (led != NULL) {
        led_strip_set_pixel(led->handle, 0, r, g, b);
        led_strip_refresh(led->handle);
    }

    xSemaphoreGive(lock);
}
