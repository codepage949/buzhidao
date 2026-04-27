#include "bridge_cls.h"

#include "bridge_image.h"
#include "bridge_predictor.h"
#include "bridge_tensor.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
namespace {

void set_cls_error_if_empty(std::string* err, const std::string& message) {
    if (err != nullptr && err->empty()) {
        *err = message;
    }
}

}  // namespace

std::pair<int, float> run_cls(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const Image& img,
    const ModelPreprocessCfg& cls_cfg,
    std::string* err
) {
    const auto input = preprocess_cls(img, cls_cfg.cls_target_w, cls_cfg.cls_target_h, cls_cfg.cls_norm);
    std::vector<float> out;
    std::vector<int> shape{1, 3, cls_cfg.cls_target_h, cls_cfg.cls_target_w};
    std::vector<int> out_shape;
    if (!run_predictor(predictor, input, shape, out, out_shape, err)) {
        set_cls_error_if_empty(err, "cls predictor 실행 실패");
        return {0, 0.0f};
    }
    if (out.empty()) {
        return {0, 0.0f};
    }
    size_t max_idx = 0;
    float max_score = out[0];
    for (size_t i = 1; i < out.size(); ++i) {
        if (out[i] > max_score) {
            max_score = out[i];
            max_idx = i;
        }
    }
    return {static_cast<int>(max_idx), max_score};
}

std::vector<std::pair<int, float>> run_cls_batch(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const std::vector<const Image*>& imgs,
    const ModelPreprocessCfg& cls_cfg,
    ClsBatchScratch* scratch,
    std::string* err
) {
    if (imgs.empty()) {
        return {};
    }

    const int batch_n = static_cast<int>(imgs.size());
    const int target_h = cls_cfg.cls_target_h;
    const int target_w = cls_cfg.cls_target_w;
    const size_t per_item = static_cast<size_t>(3 * target_h * target_w);
    const size_t batch_input_len = static_cast<size_t>(batch_n) * per_item;
    ClsBatchScratch local_scratch;
    ClsBatchScratch& buffers = scratch != nullptr ? *scratch : local_scratch;
    if (!buffers.input.ensure(batch_input_len, err)) {
        return std::vector<std::pair<int, float>>(imgs.size(), {0, 0.0f});
    }
    float* batch_input = buffers.input.get();
    for (int i = 0; i < batch_n; ++i) {
        const auto* image = imgs[static_cast<size_t>(i)];
        const Image resized = resize_bilinear(*image, target_w, target_h);
        fill_cls_tensor(
            resized,
            cls_cfg.cls_norm,
            batch_input + static_cast<size_t>(i) * per_item
        );
    }

    std::vector<int> shape{batch_n, 3, target_h, target_w};
    size_t out_len = 0;
    if (!run_predictor_into_buffer(
            predictor,
            batch_input,
            batch_input_len,
            shape,
            &buffers.output,
            &out_len,
            buffers.output_shape,
            err)) {
        set_cls_error_if_empty(err, "cls batch predictor 실행 실패");
        return std::vector<std::pair<int, float>>(imgs.size(), {0, 0.0f});
    }
    if (out_len == 0) {
        return std::vector<std::pair<int, float>>(imgs.size(), {0, 0.0f});
    }
    const float* out = buffers.output.get();

    int num_classes = 0;
    if (buffers.output_shape.size() == 2) {
        if (buffers.output_shape[0] != batch_n) {
            if (err != nullptr) {
                *err = "cls batch 출력 shape 파싱 실패";
            }
            return std::vector<std::pair<int, float>>(imgs.size(), {0, 0.0f});
        }
        num_classes = buffers.output_shape[1];
    } else if (buffers.output_shape.size() == 1 && batch_n == 1) {
        num_classes = buffers.output_shape[0];
    } else {
        if (err != nullptr) {
            *err = "cls batch 출력 shape 파싱 실패";
        }
        return std::vector<std::pair<int, float>>(imgs.size(), {0, 0.0f});
    }
    if (num_classes <= 0) {
        if (err != nullptr) {
            *err = "cls batch 출력 class 수가 유효하지 않습니다";
        }
        return std::vector<std::pair<int, float>>(imgs.size(), {0, 0.0f});
    }
    const size_t expected_values = static_cast<size_t>(batch_n) * static_cast<size_t>(num_classes);
    if (out_len < expected_values) {
        if (err != nullptr) {
            *err = "cls batch 출력 길이가 shape보다 짧습니다";
        }
        return std::vector<std::pair<int, float>>(imgs.size(), {0, 0.0f});
    }

    std::vector<std::pair<int, float>> results;
    results.reserve(imgs.size());
    for (int i = 0; i < batch_n; ++i) {
        const size_t base = static_cast<size_t>(i) * static_cast<size_t>(num_classes);
        size_t max_idx = 0;
        float max_score = out[base];
        for (int c = 1; c < num_classes; ++c) {
            const float score = out[base + static_cast<size_t>(c)];
            if (score > max_score) {
                max_score = score;
                max_idx = static_cast<size_t>(c);
            }
        }
        results.push_back({static_cast<int>(max_idx), max_score});
    }
    return results;
}

#endif
