/*
 * SPDX-FileCopyrightText: Brian Kuschak <bkuschak@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Drive the WS2812 RGB LED used on some boards.
 */

#include "led_strip.h"
#include "sdkconfig.h"

static bool initialized;
static led_strip_handle_t led_strip;

static void init_rgb_led(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_ESP_DAP_GPIO_LED,
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

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config,
                &led_strip));
    led_strip_clear(led_strip);
}

void set_rgb_led(uint8_t r, uint8_t g, uint8_t b)
{
    if(!initialized) {
        init_rgb_led();
        initialized = true;
    }
    led_strip_set_pixel(led_strip, 0, r, g, b);
    led_strip_refresh(led_strip);
}
