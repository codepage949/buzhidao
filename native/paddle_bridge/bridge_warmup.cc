#include "bridge_warmup.h"

#include "bridge_engine.h"
#include "bridge_image.h"
#include "bridge_types.h"

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
#include <paddle_inference_api.h>

std::vector<BBox> run_det(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const Image& img,
    int resize_long,
    const ModelPreprocessCfg& det_cfg,
    const NormalizeCfg& norm,
    const DetOptions& options,
    std::string* err
);

std::vector<std::pair<int, float>> run_cls_batch(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const std::vector<const Image*>& imgs,
    const ModelPreprocessCfg& cls_cfg,
    ClsBatchScratch* scratch,
    std::string* err
);

std::vector<std::pair<std::string, float>> run_rec_batch(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const std::vector<const Image*>& imgs,
    const std::vector<std::string>& dict,
    const ModelPreprocessCfg& rec_cfg,
    const std::vector<RecDebugMeta>* debug_meta,
    RecBatchScratch* scratch,
    std::string* err
);

void set_warmup_error_if_empty(std::string* err, const std::string& message) {
    if (err != nullptr && err->empty()) {
        *err = message;
    }
}

bool warmup_det_predictor(buzhi_ocr_engine* engine, std::string* err) {
    Image det_image = make_warmup_pattern_image(320, 320);
    std::string det_err;
    run_det(
        engine->det_predictor,
        det_image,
        0,
        engine->det_cfg,
        engine->det_cfg.det_norm,
        engine->det_options,
        &det_err
    );
    if (!det_err.empty()) {
        set_warmup_error_if_empty(err, det_err);
        return false;
    }
    return true;
}

bool warmup_cls_predictor(buzhi_ocr_engine* engine, std::string* err) {
    std::vector<Image> cls_inputs;
    cls_inputs.reserve(6);
    std::vector<const Image*> cls_ptrs;
    cls_ptrs.reserve(6);
    for (int i = 0; i < 6; ++i) {
        cls_inputs.push_back(make_warmup_pattern_image(96 + i * 16, 48 + (i % 2) * 8));
    }
    for (const auto& image : cls_inputs) {
        cls_ptrs.push_back(&image);
    }
    std::string cls_err;
    const auto cls_results = run_cls_batch(
        engine->cls_predictor,
        cls_ptrs,
        engine->cls_cfg,
        nullptr,
        &cls_err);
    if (!cls_err.empty()) {
        set_warmup_error_if_empty(err, cls_err);
        return false;
    }
    if (cls_results.size() != cls_ptrs.size()) {
        set_warmup_error_if_empty(err, "cls warmup 결과 수가 입력 수와 다릅니다");
        return false;
    }
    return true;
}

bool warmup_rec_predictor(buzhi_ocr_engine* engine, std::string* err) {
    std::vector<Image> rec_inputs;
    rec_inputs.reserve(6);
    std::vector<const Image*> rec_ptrs;
    rec_ptrs.reserve(6);
    constexpr std::array<int, 6> kWarmupWidths{{80, 128, 192, 256, 320, 384}};
    for (int width : kWarmupWidths) {
        rec_inputs.push_back(make_warmup_pattern_image(width, engine->rec_cfg.rec_target_h));
    }
    for (const auto& image : rec_inputs) {
        rec_ptrs.push_back(&image);
    }
    std::string rec_err;
    const auto rec_results = run_rec_batch(
        engine->rec_predictor,
        rec_ptrs,
        engine->rec_dict,
        engine->rec_cfg,
        nullptr,
        nullptr,
        &rec_err
    );
    if (!rec_err.empty()) {
        set_warmup_error_if_empty(err, rec_err);
        return false;
    }
    if (rec_results.size() != rec_ptrs.size()) {
        set_warmup_error_if_empty(err, "rec warmup 결과 수가 입력 수와 다릅니다");
        return false;
    }
    return true;
}
#endif
