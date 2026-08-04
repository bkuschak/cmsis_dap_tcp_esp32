#ifndef WS2812_LED_H
#define WS2812_LED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Set the color of the WS2812 LED on the given GPIO.
void set_rgb_led(int gpio, uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif

#endif
