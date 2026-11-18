/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GPIO_RESERVED_H
#define GPIO_RESERVED_H

#include <stdbool.h>
#include <stdint.h>

// esp_gpio_is_reserved() is declared in a private ESP-IDF header that may
// change. If the header is gone, still build, but disable the flash/PSRAM
// reserved-pin check and say so loudly (here and at runtime).
#if __has_include("esp_private/esp_gpio_reserve.h")
#include "esp_private/esp_gpio_reserve.h"
#define GPIO_RESERVED_CHECK_AVAILABLE 1
static inline bool gpio_pin_reserved(uint64_t mask)
{
    return esp_gpio_is_reserved(mask);
}
#else
// Not #warning or #pragma GCC warning: ESP-IDF builds use -Werror, so those
// would fail the build. GCC reports a message pragma as a note, but clang
// makes it a warning (fatal under -Werror), so skip it there; the runtime
// messages still apply.
#if defined(__GNUC__) && !defined(__clang__)
#pragma message("esp_private/esp_gpio_reserve.h not found: GPIO reserved-pin check is DISABLED")
#endif
#define GPIO_RESERVED_CHECK_AVAILABLE 0
static inline bool gpio_pin_reserved(uint64_t mask)
{
    (void)mask;
    return false;
}
#endif

#endif
