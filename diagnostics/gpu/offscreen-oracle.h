// SPDX-License-Identifier: Apache-2.0
// Independent CPU expectations for private, bottom-left-origin GL buffers.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace offscreen {
constexpr int kAtlas = 64;
struct Rect { int x, y, w, h; };
struct Pixel { uint8_t r, g, b, a; };
static_assert(sizeof(Pixel) == 4, "RGBA byte layout");

inline uint8_t glyph(int x, int y, int phase) {
    // Eight rows of an A, including blank pixels. The four opacity levels
    // exercise transparent and partially covered glyphs with known values.
    constexpr uint8_t rows[] = {0x18, 0x24, 0x42, 0x7e, 0x42, 0x42, 0x42, 0};
    constexpr uint8_t coverage[] = {0, 85, 170, 255};
    const int gx = (x + phase) & 7;
    return (rows[y & 7] & (0x80u >> gx))
        ? coverage[((x / 8) + (y / 8) + phase) & 3] : 0;
}
inline int texel(int p, int extent) {
    // Exact GL_NEAREST selection at pixel centers; no floating-point oracle.
    // Match the small positive vertex-coordinate bias. Full-resolution pixel
    // centers can otherwise land exactly on a texel boundary, where interpolation
    // rounding could select either neighbor and produce a false diagnosis.
    return static_cast<int>((int64_t(2 * p + 1) * 4096 + 2 * extent) * kAtlas /
                            (int64_t(2 * extent) * 4096));
}
inline uint8_t over(uint8_t destination, uint8_t source, uint8_t alpha) {
    return static_cast<uint8_t>((int(source) * alpha + int(destination) * (255 - alpha) + 127) / 255);
}
inline Pixel blend(Pixel destination, int channel, uint8_t alpha) {
    return {over(destination.r, channel == 0 ? 255 : 0, alpha),
            over(destination.g, channel == 1 ? 255 : 0, alpha),
            over(destination.b, channel == 2 ? 255 : 0, alpha), 255};
}
inline Rect damage(int frame, int width, int height) {
    if (frame % 4 == 0) return {0, 0, width, height};
    const int w = width / 2 + 3, h = height / 2 + 1;
    return {(frame * 17) % (width - w + 1),
            (frame * 11) % (height - h + 1), w, h};
}
inline void apply(std::vector<Pixel>& target, const std::vector<uint8_t>& atlas,
                  int width, int height, Rect rect, Pixel background,
                  int channel, bool clear) {
    for (int y = rect.y; y < rect.y + rect.h; ++y) {
        for (int x = rect.x; x < rect.x + rect.w; ++x) {
            const size_t p = static_cast<size_t>(y) * width + x;
            const uint8_t alpha = atlas[static_cast<size_t>(texel(y, height)) * kAtlas + texel(x, width)];
            target[p] = blend(clear ? background : target[p], channel, alpha);
        }
    }
}
struct Comparison { size_t pixels = 0, mismatches = 0, first = 0; int max_error = 0; };
inline Comparison compare(const std::vector<Pixel>& actual,
                          const std::vector<Pixel>& expected, int tolerance = 3) {
    Comparison out;
    out.pixels = expected.size();
    if (actual.size() != expected.size()) { out.mismatches = expected.size() + 1; return out; }
    for (size_t i = 0; i < actual.size(); ++i) {
        const int errors[] = {int(actual[i].r) - expected[i].r,
                              int(actual[i].g) - expected[i].g,
                              int(actual[i].b) - expected[i].b,
                              int(actual[i].a) - expected[i].a};
        bool bad = false;
        for (int error : errors) {
            const int absolute = error < 0 ? -error : error;
            if (absolute > out.max_error) out.max_error = absolute;
            if (absolute > tolerance) bad = true;
        }
        if (bad) { if (!out.mismatches) out.first = i; ++out.mismatches; }
    }
    return out;
}
inline uint32_t hash(const std::vector<Pixel>& pixels) {
    uint32_t out = 2166136261u;
    for (Pixel p : pixels) {
        for (uint8_t v : {p.r, p.g, p.b, p.a}) { out ^= v; out *= 16777619u; }
    }
    return out;
}
} // namespace offscreen
