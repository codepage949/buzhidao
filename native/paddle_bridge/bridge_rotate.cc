#include "bridge_rotate.h"

#include "bridge_image.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#if __has_include(<opencv2/opencv.hpp>)
#include <opencv2/opencv.hpp>
#define BUZHIDAO_HAVE_OPENCV 1
#endif

Image rotate180(const Image& img) {
#if defined(BUZHIDAO_HAVE_OPENCV)
    cv::Mat src = image_to_cv_mat_bgr(img);
    const int h = img.height;
    const int w = img.width;
    const cv::Point2f center(static_cast<float>(w) / 2.0f, static_cast<float>(h) / 2.0f);
    cv::Mat rotation = cv::getRotationMatrix2D(center, 180.0, 1.0);
    const double cos_v = std::abs(rotation.at<double>(0, 0));
    const double sin_v = std::abs(rotation.at<double>(0, 1));
    const int new_w = static_cast<int>((static_cast<double>(h) * sin_v) + (static_cast<double>(w) * cos_v));
    const int new_h = static_cast<int>((static_cast<double>(h) * cos_v) + (static_cast<double>(w) * sin_v));
    rotation.at<double>(0, 2) += (static_cast<double>(new_w) - static_cast<double>(w)) / 2.0;
    rotation.at<double>(1, 2) += (static_cast<double>(new_h) - static_cast<double>(h)) / 2.0;
    cv::Mat rotated;
    cv::warpAffine(
        src,
        rotated,
        rotation,
        cv::Size(new_w, new_h),
        cv::INTER_CUBIC,
        cv::BORDER_CONSTANT
    );
    return cv_mat_to_image_bgra(rotated);
#else
    auto cubic_weight = [](float x) {
        constexpr float a = -0.75f;
        x = std::fabs(x);
        if (x <= 1.0f) {
            return ((a + 2.0f) * x - (a + 3.0f)) * x * x + 1.0f;
        }
        if (x < 2.0f) {
            return (((a * x - 5.0f * a) * x + 8.0f * a) * x) - 4.0f * a;
        }
        return 0.0f;
    };
    auto sample_channel_cubic_constant_zero = [&](float sx, float sy, int channel) {
        const int base_x = static_cast<int>(std::floor(sx));
        const int base_y = static_cast<int>(std::floor(sy));
        float accum = 0.0f;
        float weight_sum = 0.0f;
        for (int dy = -1; dy <= 2; ++dy) {
            const int py = base_y + dy;
            const float wy = cubic_weight(sy - static_cast<float>(base_y + dy));
            for (int dx = -1; dx <= 2; ++dx) {
                const int px = base_x + dx;
                const float wx = cubic_weight(sx - static_cast<float>(base_x + dx));
                const float w = wx * wy;
                if (px >= 0 && px < img.width && py >= 0 && py < img.height) {
                    const size_t idx = (static_cast<size_t>(py) * img.width + px) * 4 + channel;
                    accum += w * static_cast<float>(img.pixels[idx]);
                }
                weight_sum += w;
            }
        }
        const float value = weight_sum > 0.0f ? (accum / weight_sum) : 0.0f;
        return static_cast<uint8_t>(std::clamp(static_cast<float>(std::round(value)), 0.0f, 255.0f));
    };
    Image out{img.width, img.height, img.channels, std::vector<uint8_t>(static_cast<size_t>(img.width * img.height) * 4, 0)};
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            const float sx = static_cast<float>(img.width - x);
            const float sy = static_cast<float>(img.height - y);
            const size_t dst = static_cast<size_t>(y * img.width + x) * 4;
            out.pixels[dst + 0] = sample_channel_cubic_constant_zero(sx, sy, 0);
            out.pixels[dst + 1] = sample_channel_cubic_constant_zero(sx, sy, 1);
            out.pixels[dst + 2] = sample_channel_cubic_constant_zero(sx, sy, 2);
            out.pixels[dst + 3] = 255;
        }
    }
    return out;
#endif
}

Image rotate90_counterclockwise(const Image& img) {
    Image out{img.height, img.width, img.channels, std::vector<uint8_t>(static_cast<size_t>(img.width * img.height) * 4)};
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            const int nx = y;
            const int ny = img.width - 1 - x;
            const size_t src = static_cast<size_t>(y * img.width + x) * 4;
            const size_t dst = static_cast<size_t>(ny * out.width + nx) * 4;
            out.pixels[dst + 0] = img.pixels[src + 0];
            out.pixels[dst + 1] = img.pixels[src + 1];
            out.pixels[dst + 2] = img.pixels[src + 2];
            out.pixels[dst + 3] = img.pixels[src + 3];
        }
    }
    return out;
}
