#include "bridge_tensor.h"

#include "bridge_image.h"
#include "bridge_utils.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

void fill_cls_tensor(const Image& resized, const NormalizeCfg& norm, float* dst) {
    const int resized_w = resized.width;
    const int resized_h = resized.height;
    const float cls_scale = norm.scale;
    const float cls_std0 = std::fabs(norm.std[0]) > 1e-12f ? norm.std[0] : 1.0f;
    const float cls_std1 = std::fabs(norm.std[1]) > 1e-12f ? norm.std[1] : 1.0f;
    const float cls_std2 = std::fabs(norm.std[2]) > 1e-12f ? norm.std[2] : 1.0f;
    const size_t hw_stride = static_cast<size_t>(resized_w * resized_h);
    for (int y = 0; y < resized.height; ++y) {
        for (int x = 0; x < resized.width; ++x) {
            const size_t idx = static_cast<size_t>((y * resized.width + x) * 4);
            const float r = image_red_at(resized, idx) * cls_scale - norm.mean[0];
            const float g = image_green_at(resized, idx) * cls_scale - norm.mean[1];
            const float b = image_blue_at(resized, idx) * cls_scale - norm.mean[2];
            const size_t hw = static_cast<size_t>(y * resized.width + x);
            dst[0 * hw_stride + hw] = r / cls_std0;
            dst[1 * hw_stride + hw] = g / cls_std1;
            dst[2 * hw_stride + hw] = b / cls_std2;
        }
    }
}

void fill_rec_tensor(const Image& resized, const NormalizeCfg& norm, int tensor_w, float* dst) {
    const float rec_scale = norm.scale;
    const float rec_std0 = std::fabs(norm.std[0]) > 1e-12f ? norm.std[0] : 1.0f;
    const float rec_std1 = std::fabs(norm.std[1]) > 1e-12f ? norm.std[1] : 1.0f;
    const float rec_std2 = std::fabs(norm.std[2]) > 1e-12f ? norm.std[2] : 1.0f;
    const float pad_b = 0.0f;
    const float pad_g = 0.0f;
    const float pad_r = 0.0f;
    if (debug_enabled()) {
        debug_log("preprocess_rec pad_values=" +
                  std::to_string(pad_b) + "," +
                  std::to_string(pad_g) + "," +
                  std::to_string(pad_r));
    }
    const size_t hw_stride = static_cast<size_t>(tensor_w * resized.height);
    std::fill(dst, dst + hw_stride, pad_b);
    std::fill(dst + hw_stride, dst + hw_stride * 2u, pad_g);
    std::fill(dst + hw_stride * 2u, dst + hw_stride * 3u, pad_r);
    for (int y = 0; y < resized.height; ++y) {
        for (int x = 0; x < resized.width; ++x) {
            const size_t idx = static_cast<size_t>((y * resized.width + x) * 4);
            if (resized.pixels[idx + 3] == 0) {
                continue;
            }
            const size_t hw = static_cast<size_t>(y * tensor_w + x);
            const float b = image_blue_at(resized, idx) * rec_scale - norm.mean[0];
            const float g = image_green_at(resized, idx) * rec_scale - norm.mean[1];
            const float r = image_red_at(resized, idx) * rec_scale - norm.mean[2];
            dst[0 * hw_stride + hw] = b / rec_std0;
            dst[1 * hw_stride + hw] = g / rec_std1;
            dst[2 * hw_stride + hw] = r / rec_std2;
        }
    }
}

std::vector<float> preprocess_det(
    const Image& img,
    int det_limit_side_len,
    const std::string& det_limit_type,
    int det_max_side_limit,
    const NormalizeCfg& norm,
    int* out_h,
    int* out_w
) {
    const Image resized = resize_for_det(
        img,
        det_limit_side_len,
        det_limit_type,
        det_max_side_limit,
        out_h,
        out_w
    );
    if (debug_enabled()) {
        debug_log(std::string("preprocess_det src=") + std::to_string(img.width) + "x" +
                  std::to_string(img.height) + ", limit_side_len=" + std::to_string(det_limit_side_len) +
                  ", limit_type=" + det_limit_type +
                  ", max_side_limit=" + std::to_string(det_max_side_limit) +
                  ", resized=" + std::to_string(*out_w) + "x" + std::to_string(*out_h));
    }
    std::vector<float> tensor(1u * 3u * static_cast<size_t>(resized.width * resized.height));
    const float det_scale = norm.scale;
    const float det_std0 = std::fabs(norm.std[0]) > 1e-12f ? norm.std[0] : 1.0f;
    const float det_std1 = std::fabs(norm.std[1]) > 1e-12f ? norm.std[1] : 1.0f;
    const float det_std2 = std::fabs(norm.std[2]) > 1e-12f ? norm.std[2] : 1.0f;
    for (int y = 0; y < resized.height; ++y) {
        for (int x = 0; x < resized.width; ++x) {
            const size_t idx = static_cast<size_t>((y * resized.width + x) * 4);
            const float b = image_blue_at(resized, idx) * det_scale - norm.mean[0];
            const float g = image_green_at(resized, idx) * det_scale - norm.mean[1];
            const float r = image_red_at(resized, idx) * det_scale - norm.mean[2];
            const size_t hw = static_cast<size_t>(y * resized.width + x);
            const size_t hw_stride = static_cast<size_t>(resized.width * resized.height);
            tensor[0 * hw_stride + hw] = b / det_std0;
            tensor[1 * hw_stride + hw] = g / det_std1;
            tensor[2 * hw_stride + hw] = r / det_std2;
        }
    }
    if (debug_enabled() && !tensor.empty()) {
        auto mn = tensor[0];
        auto mx = tensor[0];
        double sum = 0.0;
        double sumsq = 0.0;
        for (const float v : tensor) {
            mn = std::min(mn, v);
            mx = std::max(mx, v);
            sum += v;
            sumsq += v * v;
        }
        const size_t center = tensor.size() / 2;
        debug_log(std::string("preprocess_det tensor stats mn=") + std::to_string(mn) +
                  ", mx=" + std::to_string(mx) + ", mean=" + std::to_string(sum / tensor.size()) +
                  ", rms=" + std::to_string(std::sqrt(sumsq / tensor.size())) +
                  ", center=" + std::to_string(tensor[center]));
    }
    return tensor;
}

std::vector<float> preprocess_cls(const Image& img, int target_w, int target_h, const NormalizeCfg& norm) {
    const int resized_w = std::max(1, target_w);
    const int resized_h = std::max(1, target_h);
    const Image resized = resize_bilinear(img, resized_w, resized_h);
    std::vector<float> tensor(1u * 3u * resized_h * resized_w);
    fill_cls_tensor(resized, norm, tensor.data());
    return tensor;
}

Image resize_rec_input_image(
    const Image& img,
    int target_h,
    int target_w,
    int max_w,
    int& out_w
) {
    const int rec_h = std::max(1, target_h);
    const int rec_base_w = std::max(1, target_w);
    const int rec_max_w = std::max(rec_base_w, max_w);
    const float ratio = static_cast<float>(img.width) / std::max(1, img.height);
    const float base_ratio = static_cast<float>(rec_base_w) / rec_h;
    const float max_wh_ratio = std::max(base_ratio, ratio);
    int dynamic_w = static_cast<int>(rec_h * max_wh_ratio);
    dynamic_w = std::max(rec_base_w, dynamic_w);
    dynamic_w = std::min(rec_max_w, dynamic_w);
    int resized_w = 0;
    if (dynamic_w >= rec_max_w) {
        resized_w = rec_max_w;
        dynamic_w = rec_max_w;
    } else if (static_cast<int>(std::ceil(rec_h * ratio)) > dynamic_w) {
        resized_w = dynamic_w;
    } else {
        resized_w = static_cast<int>(std::ceil(rec_h * ratio));
    }
    resized_w = std::max(1, resized_w);
    const Image resized = resize_bilinear(img, resized_w, rec_h);
    out_w = dynamic_w;
    return resized;
}

std::vector<float> preprocess_rec(
    const Image& img,
    int target_h,
    int target_w,
    int max_w,
    const NormalizeCfg& norm,
    int& out_w
) {
    const Image resized = resize_rec_input_image(img, target_h, target_w, max_w, out_w);
    std::vector<float> tensor(1u * 3u * static_cast<size_t>(resized.height * out_w), 0.0f);
    fill_rec_tensor(resized, norm, out_w, tensor.data());
    return tensor;
}
