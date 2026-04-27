#include "bridge_resample.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

float cubic_weight(float x) {
    constexpr float a = -0.75f;
    x = std::fabs(x);
    if (x <= 1.0f) {
        return ((a + 2.0f) * x - (a + 3.0f)) * x * x + 1.0f;
    }
    if (x < 2.0f) {
        return (((a * x - 5.0f * a) * x + 8.0f * a) * x) - 4.0f * a;
    }
    return 0.0f;
}

uint8_t sample_channel_cubic_replicate(const Image& img, float sx, float sy, int channel) {
    const int base_x = static_cast<int>(std::floor(sx));
    const int base_y = static_cast<int>(std::floor(sy));
    float accum = 0.0f;
    float weight_sum = 0.0f;
    for (int dy = -1; dy <= 2; ++dy) {
        const int py = std::clamp(base_y + dy, 0, img.height - 1);
        const float wy = cubic_weight(sy - static_cast<float>(base_y + dy));
        for (int dx = -1; dx <= 2; ++dx) {
            const int px = std::clamp(base_x + dx, 0, img.width - 1);
            const float wx = cubic_weight(sx - static_cast<float>(base_x + dx));
            const float w = wx * wy;
            const size_t idx = (static_cast<size_t>(py) * img.width + px) * 4 + channel;
            accum += w * static_cast<float>(img.pixels[idx]);
            weight_sum += w;
        }
    }
    const float value = weight_sum > 0.0f ? (accum / weight_sum) : 0.0f;
    return static_cast<uint8_t>(std::clamp(static_cast<float>(std::round(value)), 0.0f, 255.0f));
}

uint8_t sample_channel_bilinear_replicate(const Image& img, float sx, float sy, int channel) {
    const float gx = std::clamp(sx, 0.0f, static_cast<float>(img.width - 1));
    const float gy = std::clamp(sy, 0.0f, static_cast<float>(img.height - 1));
    const int x0 = std::clamp(static_cast<int>(std::floor(gx)), 0, img.width - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(gy)), 0, img.height - 1);
    const int x1 = std::clamp(x0 + 1, 0, img.width - 1);
    const int y1 = std::clamp(y0 + 1, 0, img.height - 1);
    const float wx = std::clamp(gx - std::floor(gx), 0.0f, 1.0f);
    const float wy = std::clamp(gy - std::floor(gy), 0.0f, 1.0f);
    const float wx1 = 1.0f - wx;
    const float wy1 = 1.0f - wy;
    const size_t d0 = static_cast<size_t>((y0 * img.width + x0) * 4 + channel);
    const size_t d1 = static_cast<size_t>((y0 * img.width + x1) * 4 + channel);
    const size_t d2 = static_cast<size_t>((y1 * img.width + x0) * 4 + channel);
    const size_t d3 = static_cast<size_t>((y1 * img.width + x1) * 4 + channel);
    const float p0 = img.pixels[d0] * wx1 + img.pixels[d1] * wx;
    const float p1 = img.pixels[d2] * wx1 + img.pixels[d3] * wx;
    const float p = p0 * wy1 + p1 * wy;
    return static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::round(p)), 0, 255));
}
