#include "bridge.h"
#include "bridge_engine.h"
#include "bridge_output.h"
#include "bridge_pipeline.h"
#include "bridge_warmup.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

void set_api_error(char** err, const std::string& message) {
    if (err != nullptr) {
        *err = dup_string(message);
    }
}

constexpr const char* kPaddleNotLinkedMessage =
    "Paddle FFI bridge skeleton is compiled, but Paddle Inference SDK is not linked. Download and configure paddle_inference package under .paddle_inference.";

float clamp_score_threshold(float score_thresh) {
    if (score_thresh < 0.0f) {
        return 0.0f;
    }
    if (score_thresh > 1.0f) {
        return 1.0f;
    }
    return score_thresh;
}

}  // namespace

extern "C" void buzhi_ocr_destroy(buzhi_ocr_engine_t* engine) {
    delete engine;
}

extern "C" int buzhi_ocr_warmup_predictors(
    buzhi_ocr_engine_t* engine,
    char** err
) {
    if (engine == nullptr) {
        set_api_error(err, "engine is null");
        return 0;
    }

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
    std::string warmup_err;
    if (!warmup_det_predictor(engine, &warmup_err) ||
        !warmup_cls_predictor(engine, &warmup_err) ||
        !warmup_rec_predictor(engine, &warmup_err)) {
        set_api_error(err, warmup_err);
        return 0;
    }
    return 1;
#else
    set_api_error(err, kPaddleNotLinkedMessage);
    return 0;
#endif
}

extern "C" buzhi_ocr_result_t* buzhi_ocr_run_image_file_result(
    buzhi_ocr_engine_t* engine,
    const char* image_path,
    int det_resize_long,
    float score_thresh,
    int debug_trace,
    char** err
) {
    if (engine == nullptr) {
        set_api_error(err, "engine is null");
        return nullptr;
    }
    if (image_path == nullptr || image_path[0] == '\0') {
        set_api_error(err, "image_path is empty");
        return nullptr;
    }
    score_thresh = clamp_score_threshold(score_thresh);

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
    PipelineOutput result = run_pipeline_from_path(
        engine,
        fs::path(image_path),
        det_resize_long,
        score_thresh,
        debug_trace,
        err
    );
    return build_native_result(std::move(result), debug_trace != 0);
#else
    set_api_error(err, kPaddleNotLinkedMessage);
    return nullptr;
#endif
}

extern "C" buzhi_ocr_result_t* buzhi_ocr_run_image_rgba_result(
    buzhi_ocr_engine_t* engine,
    const unsigned char* rgba,
    int width,
    int height,
    int stride,
    int det_resize_long,
    float score_thresh,
    int debug_trace,
    char** err
) {
    if (engine == nullptr) {
        set_api_error(err, "engine is null");
        return nullptr;
    }
    if (rgba == nullptr) {
        set_api_error(err, "rgba is null");
        return nullptr;
    }
    if (width <= 0 || height <= 0) {
        set_api_error(err, "image size is invalid");
        return nullptr;
    }
    if (stride < width * 4) {
        set_api_error(err, "image stride is invalid");
        return nullptr;
    }
    score_thresh = clamp_score_threshold(score_thresh);

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
    Image img{
        width,
        height,
        4,
        std::vector<uint8_t>(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u),
        PixelLayout::RGBA
    };
    for (int y = 0; y < height; ++y) {
        const auto* src = rgba + static_cast<size_t>(y) * static_cast<size_t>(stride);
        auto* dst = img.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u;
        std::memcpy(dst, src, static_cast<size_t>(width) * 4u);
    }

    PipelineOutput result = run_pipeline(
        engine,
        img,
        "rgba_memory",
        det_resize_long,
        score_thresh,
        debug_trace,
        err
    );
    return build_native_result(std::move(result), debug_trace != 0);
#else
    set_api_error(err, kPaddleNotLinkedMessage);
    return nullptr;
#endif
}
