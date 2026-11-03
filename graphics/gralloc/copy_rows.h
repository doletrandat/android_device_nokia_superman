// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Callers validate span bounds. Never copy padding from unequal strides.
static inline void superman_copy_rows(uint8_t* dest, size_t destStride,
                                     const uint8_t* source, size_t sourceStride,
                                     size_t rowBytes, size_t rows) {
    if (destStride == rowBytes && sourceStride == rowBytes) {
        std::memcpy(dest, source, rowBytes * rows);
    } else {
        for (size_t y = 0; y < rows; ++y)
            std::memcpy(dest + y * destStride, source + y * sourceStride, rowBytes);
    }
}

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

// Wide, sequential loads for GPU write-combined mappings. The ordinary Cortex-A7
// libc memcpy uses doubleword loads for large aligned rows; benchmark before use.
static inline void superman_copy_rows_streaming(uint8_t* dest, size_t destStride,
                                               const uint8_t* source, size_t sourceStride,
                                               size_t rowBytes, size_t rows) {
#if defined(__ARM_NEON)
    for (size_t y = 0; y < rows; ++y) {
        uint8_t* d = dest + y * destStride;
        const uint8_t* s = source + y * sourceStride;
        size_t x = 0;
        for (; rowBytes - x >= 64; x += 64) {
            const uint8x16_t a = vld1q_u8(s + x);
            const uint8x16_t b = vld1q_u8(s + x + 16);
            const uint8x16_t c = vld1q_u8(s + x + 32);
            const uint8x16_t e = vld1q_u8(s + x + 48);
            vst1q_u8(d + x, a); vst1q_u8(d + x + 16, b);
            vst1q_u8(d + x + 32, c); vst1q_u8(d + x + 48, e);
        }
        if (x < rowBytes) std::memcpy(d + x, s + x, rowBytes - x);
    }
#else
    superman_copy_rows(dest, destStride, source, sourceStride, rowBytes, rows);
#endif
}
