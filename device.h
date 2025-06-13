#ifndef DEVICE_H
#define DEVICE_H

#include <stdint.h>

// Don't use ARM assembler
#define __CC_ARM

#define __STATIC_INLINE static inline
#define __STATIC_FORCEINLINE static inline
#define __ASM asm
#define __WEAK __attribute__((weak))

#endif
