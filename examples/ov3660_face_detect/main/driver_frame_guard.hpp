#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

// Detect the observed RGB565 byte-phase fault, not arbitrary scene changes.
// A shifted high/low byte makes neighbouring pixels wildly different; reading
// the opposite byte order becomes much smoother. Motion/exposure changes do
// not cause that within-frame signature. Never "repair" data used for AI.
struct DriverFrameQuality {
    uint32_t normal_delta = 0;
    uint32_t swapped_delta = 0;
    unsigned bad_rows = 0;
    bool valid = true;
};

inline DriverFrameQuality driver_frame_quality(const uint8_t *data, unsigned width, unsigned height)
{
    DriverFrameQuality quality;
    if (!data || width < 16 || height < 16) {
        quality.valid = false;
        return quality;
    }
    const auto difference = [](uint16_t a, uint16_t b) -> unsigned {
        return (std::abs(int((a >> 11) & 31) - int((b >> 11) & 31)) * 255 / 31 +
                std::abs(int((a >> 5) & 63) - int((b >> 5) & 63)) * 255 / 63 +
                std::abs(int(a & 31) - int(b & 31)) * 255 / 31) / 3;
    };
    unsigned rows = 0;
    for (unsigned y = 4; y + 4 < height; y += 10) {
        unsigned normal = 0, swapped = 0, samples = 0;
        for (unsigned x = 4; x + 2 < width; x += 12) {
            const size_t offset = (size_t(y) * width + x) * 2;
            const uint16_t a = (uint16_t(data[offset]) << 8) | data[offset + 1];
            const uint16_t b = (uint16_t(data[offset + 2]) << 8) | data[offset + 3];
            normal += difference(a, b);
            swapped += difference(uint16_t((a << 8) | (a >> 8)), uint16_t((b << 8) | (b >> 8)));
            ++samples;
        }
        normal /= samples;
        swapped /= samples;
        quality.normal_delta += normal;
        quality.swapped_delta += swapped;
        quality.bad_rows += normal >= 40 && swapped * 100 < normal * 45;
        ++rows;
    }
    quality.normal_delta /= rows;
    quality.swapped_delta /= rows;
    // Two sampled rows are needed; a single detailed edge is insufficient.
    quality.valid = quality.bad_rows < 2;
    return quality;
}
