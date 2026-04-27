#include "bridge_pipeline.h"

#include "bridge_cls.h"
#include "bridge_crop.h"
#include "bridge_debug_dump.h"
#include "bridge_debug_format.h"
#include "bridge_det.h"
#include "bridge_engine.h"
#include "bridge_geometry.h"
#include "bridge_image.h"
#include "bridge_output.h"
#include "bridge_rec.h"
#include "bridge_rec_pipeline.h"
#include "bridge_rotate.h"
#include "bridge_utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

#if __has_include(<opencv2/opencv.hpp>)
#include <opencv2/opencv.hpp>
#define BUZHIDAO_HAVE_OPENCV 1
#endif

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
namespace {

void set_pipeline_error(char** err, const std::string& message) {
    if (err != nullptr) {
        *err = dup_string(message);
    }
}

}  // namespace

bool collect_cls_inputs(
    const Image& img,
    const std::vector<BBox>& boxes,
    bool need_rec_debug_meta,
    double* crop_ms,
    std::vector<ClsPreparedInput>* cls_inputs,
    char** err
#if defined(BUZHIDAO_HAVE_OPENCV)
    ,
    const cv::Mat& original_bgr
#endif
) {
#if defined(BUZHIDAO_HAVE_OPENCV)
    (void)img;
#endif
    if (cls_inputs == nullptr) {
        set_pipeline_error(err, "OCR cls 입력 버퍼가 없습니다");
        return false;
    }
    const bool profile_stages = profile_stages_enabled();
    size_t skipped_crops = 0;
    cls_inputs->clear();
    cls_inputs->reserve(boxes.size());
    for (const auto& b : boxes) {
        std::string crop_err;
        const auto crop_started = std::chrono::steady_clock::now();
#if defined(BUZHIDAO_HAVE_OPENCV)
        Image cropped = crop_to_bbox(original_bgr, b.pts, &crop_err);
#else
        Image cropped = crop_to_bbox(img, b.pts, &crop_err);
#endif
        if (profile_stages && crop_ms != nullptr) {
            *crop_ms += elapsed_ms_since(crop_started);
        }
        if (cropped.width <= 0 || cropped.height <= 0) {
            ++skipped_crops;
            if (!crop_err.empty()) {
                debug_log_lazy([&]() { return "collect_cls_inputs crop skipped: " + crop_err; });
            }
            continue;
        }
        std::array<FloatPoint, 4> crop_quad{};
        if (need_rec_debug_meta) {
            const auto [described_crop_quad, _expected_crop_w, _expected_crop_h] = describe_crop_to_bbox(b.pts);
            crop_quad = described_crop_quad;
        }
        cls_inputs->push_back({std::move(cropped), b.pts, crop_quad});
    }
    if (!boxes.empty() && cls_inputs->empty()) {
        set_pipeline_error(err, "det box는 있지만 유효한 crop을 하나도 만들지 못했습니다");
        return false;
    }
    if (skipped_crops > 0) {
        debug_log_lazy([&]() {
            return "collect_cls_inputs skipped_crops=" + std::to_string(skipped_crops) +
                   "/" + std::to_string(boxes.size());
        });
    }
    return true;
}

bool run_cls_batches_into(
    buzhi_ocr_engine* engine,
    const std::vector<ClsPreparedInput>& cls_inputs,
    double* cls_ms,
    std::vector<std::pair<int, float>>* cls_results,
    char** err
) {
    if (cls_results == nullptr) {
        set_pipeline_error(err, "OCR cls 결과 버퍼가 없습니다");
        return false;
    }
    const bool profile_stages = profile_stages_enabled();
    cls_results->assign(cls_inputs.size(), {0, 0.0f});
    constexpr size_t kClsBatchSize = 6;
    for (size_t start = 0; start < cls_inputs.size(); start += kClsBatchSize) {
        const size_t end = std::min(cls_inputs.size(), start + kClsBatchSize);
        std::vector<const Image*> batch_images;
        batch_images.reserve(end - start);
        for (size_t i = start; i < end; ++i) {
            batch_images.push_back(&cls_inputs[i].cropped);
        }
        std::string cls_err;
        const auto cls_started = std::chrono::steady_clock::now();
        const auto batch_results = run_cls_batch(engine->cls_predictor, batch_images, engine->cls_cfg, &cls_err);
        if (profile_stages && cls_ms != nullptr) {
            *cls_ms += elapsed_ms_since(cls_started);
        }
        if (!cls_err.empty()) {
            set_pipeline_error(err, cls_err);
            return false;
        }
        if (batch_results.size() != end - start) {
            set_pipeline_error(err, "cls batch 결과 개수가 입력 수와 다릅니다");
            return false;
        }
        for (size_t i = start; i < end; ++i) {
            (*cls_results)[i] = batch_results[i - start];
        }
    }
    return true;
}

bool build_rec_candidates(
    std::vector<ClsPreparedInput>* cls_inputs,
    const std::vector<std::pair<int, float>>& cls_results,
    int debug_trace,
    double* rotate_ms,
    size_t* rotated_count,
    std::vector<RecCandidate>* rec_candidates,
    char** err
) {
    if (cls_inputs == nullptr || rec_candidates == nullptr) {
        set_pipeline_error(err, "OCR rec 후보 버퍼가 없습니다");
        return false;
    }
    if (cls_results.size() != cls_inputs->size()) {
        set_pipeline_error(err, "cls 결과 개수가 crop 입력 수와 다릅니다");
        return false;
    }
    const bool profile_stages = profile_stages_enabled();
    rec_candidates->clear();
    rec_candidates->reserve(cls_inputs->size());
    for (size_t i = 0; i < cls_inputs->size(); ++i) {
        auto& prepared = (*cls_inputs)[i];
        Image cropped = std::move(prepared.cropped);
        const auto cls = cls_results[i];
        if (debug_trace) {
            debug_log_lazy([&]() {
                return "cls label=" + std::to_string(cls.first) +
                       ", score=" + std::to_string(cls.second) +
                       ", crop=" + std::to_string(cropped.width) + "x" +
                       std::to_string(cropped.height);
            });
        }
        const bool rotated_180 = cls.first == 1;
        if (rotated_180) {
            const auto rotate_started = std::chrono::steady_clock::now();
            cropped = rotate180(cropped);
            if (profile_stages && rotate_ms != nullptr) {
                *rotate_ms += elapsed_ms_since(rotate_started);
            }
            if (rotated_count != nullptr) {
                ++(*rotated_count);
            }
        }
        const float ratio = static_cast<float>(cropped.width) / static_cast<float>(std::max(1, cropped.height));
        rec_candidates->push_back({
            prepared.pts,
            prepared.crop_quad,
            std::move(cropped),
            ratio,
            cls.first,
            cls.second,
            rotated_180
        });
    }
    return true;
}

bool run_rec_batches_into(
    buzhi_ocr_engine* engine,
    const std::vector<RecCandidate>& rec_candidates,
    const std::vector<size_t>& rec_order,
    const std::vector<std::pair<size_t, size_t>>& rec_batches,
    bool need_rec_debug_meta,
    std::vector<std::pair<std::string, float>>* rec_results,
    double* rec_ms,
    char** err
) {
    if (rec_results == nullptr) {
        set_pipeline_error(err, "OCR rec 결과 버퍼가 없습니다");
        return false;
    }
    const bool profile_stages = profile_stages_enabled();
    RecBatchScratch scratch;
    for (const auto& batch : rec_batches) {
        const size_t start = batch.first;
        const size_t end = batch.second;
        std::vector<const Image*> batch_images;
        batch_images.reserve(end - start);
        std::vector<RecDebugMeta> batch_meta;
        if (need_rec_debug_meta) {
            batch_meta.reserve(end - start);
        }
        for (size_t pos = start; pos < end; ++pos) {
            const size_t original_index = rec_order[pos];
            const auto& candidate = rec_candidates[original_index];
            batch_images.push_back(&candidate.cropped);
            if (need_rec_debug_meta) {
                batch_meta.push_back({
                    original_index,
                    candidate.pts,
                    candidate.crop_quad,
                    candidate.ratio,
                    candidate.cls_label,
                    candidate.cls_score,
                    candidate.rotated_180,
                    candidate.cropped.width,
                    candidate.cropped.height
                });
            }
        }
        std::string rec_err;
        const auto rec_started = std::chrono::steady_clock::now();
        const auto batch_results = run_rec_batch(
            engine->rec_predictor,
            batch_images,
            engine->rec_dict,
            engine->rec_cfg,
            need_rec_debug_meta ? &batch_meta : nullptr,
            &scratch,
            &rec_err
        );
        if (profile_stages && rec_ms != nullptr) {
            *rec_ms += elapsed_ms_since(rec_started);
        }
        if (!rec_err.empty()) {
            set_pipeline_error(err, rec_err);
            return false;
        }
        if (batch_results.size() != end - start) {
            set_pipeline_error(err, "rec batch 결과 개수가 입력 수와 다릅니다");
            return false;
        }
        for (size_t pos = start; pos < end; ++pos) {
            (*rec_results)[rec_order[pos]] = batch_results[pos - start];
        }
    }
    return true;
}

bool append_pipeline_results(
    PipelineOutput* output,
    const std::vector<RecCandidate>& rec_candidates,
    const std::vector<std::pair<std::string, float>>& rec_results,
    float score_thresh,
    int debug_trace,
    char** err
) {
    if (output == nullptr) {
        set_pipeline_error(err, "OCR 결과 버퍼가 없습니다");
        return false;
    }
    for (size_t i = 0; i < rec_candidates.size(); ++i) {
        const auto& candidate = rec_candidates[i];
        const auto& rec = rec_results[i];
        const std::string& text = rec.first;
        const float score = rec.second;
        if (debug_trace) {
            dump_candidate_crop_if_requested(candidate, "first_stage", candidate.cropped);
        }
        if (score >= score_thresh) {
            if (!append_detection(output, candidate.pts, text)) {
                set_pipeline_error(err, "OCR 결과 버퍼가 부족합니다");
                return false;
            }
            if (debug_trace && !append_debug_detection(output, candidate.pts, text, score, true)) {
                set_pipeline_error(err, "OCR 디버그 결과 버퍼가 부족합니다");
                return false;
            }
            if (debug_trace) {
                debug_log_lazy([&]() {
                    return "box score=" + std::to_string(score) +
                           ", text='" + text + "', accepted=true";
                });
                if (score < 0.5f) {
                    dump_candidate_crop_if_requested(candidate, "low_score", candidate.cropped);
                }
            }
        } else if (debug_trace) {
            if (!append_debug_detection(output, candidate.pts, text, score, false)) {
                set_pipeline_error(err, "OCR 디버그 결과 버퍼가 부족합니다");
                return false;
            }
            debug_log_lazy([&]() {
                return "box score=" + std::to_string(score) +
                       ", text='" + text + "', accepted=false, crop=" +
                       std::to_string(candidate.cropped.width) + "x" + std::to_string(candidate.cropped.height) +
                       ", polygon=" + quote_polygon(candidate.pts);
            });
            dump_candidate_crop_if_requested(candidate, "reject", candidate.cropped);
        }
    }
    return true;
}

PipelineOutput run_pipeline(
    buzhi_ocr_engine* engine,
    const Image& img,
    const std::string& image_label,
    int det_resize_long,
    float score_thresh,
    int debug_trace,
    char** err
) {
    const bool profile_stages = profile_stages_enabled();
    const auto pipeline_started = std::chrono::steady_clock::now();
    double load_ms = 0.0;
    double det_ms = 0.0;
    double crop_ms = 0.0;
    double cls_ms = 0.0;
    double rotate_ms = 0.0;
    double rec_ms = 0.0;
    double post_ms = 0.0;
    size_t rotated_count = 0;
    const char* dump_rec_logits_raw = std::getenv("BUZHIDAO_PADDLE_FFI_DUMP_REC_LOGITS");
    const bool dump_rec_logits =
        dump_rec_logits_raw != nullptr &&
        dump_rec_logits_raw[0] != '\0' &&
        std::strcmp(dump_rec_logits_raw, "0") != 0 &&
        std::strcmp(dump_rec_logits_raw, "false") != 0 &&
        std::strcmp(dump_rec_logits_raw, "FALSE") != 0;
    const bool need_rec_debug_meta = debug_trace || dump_rec_logits;

#if defined(BUZHIDAO_HAVE_OPENCV)
    const auto bgr_started = std::chrono::steady_clock::now();
    cv::Mat original_bgr = image_to_cv_mat_bgr(img);
    if (original_bgr.empty()) {
        set_pipeline_error(err, "OpenCV BGR 이미지 변환에 실패했습니다");
        return {};
    }
    if (profile_stages) {
        crop_ms += elapsed_ms_since(bgr_started);
    }
#endif

    if (engine->rec_dict.empty()) {
        set_pipeline_error(err, "recognition 사전이 없습니다. 모델 폴더에 사전(rec_dict.txt 등)을 넣어주세요.");
        return {};
    }

    std::string det_err;
    const auto det_started = std::chrono::steady_clock::now();
    const auto boxes = run_det(
        engine->det_predictor,
        img,
        det_resize_long,
        engine->det_cfg,
        engine->det_cfg.det_norm,
        engine->det_options,
        &det_err
    );
    if (profile_stages) {
        det_ms += elapsed_ms_since(det_started);
    }
    debug_log_lazy([&]() {
        return "run_pipeline: det_boxes=" + std::to_string(boxes.size());
    });
    if (!det_err.empty()) {
        set_pipeline_error(err, det_err);
        return {};
    }
    PipelineOutput output;
    std::vector<RecCandidate> rec_candidates;
    rec_candidates.reserve(boxes.size());
    reserve_pipeline_output(&output, boxes.size(), debug_trace != 0, boxes.size());
    std::vector<ClsPreparedInput> cls_inputs;
    if (!collect_cls_inputs(
            img,
            boxes,
            need_rec_debug_meta,
            &crop_ms,
            &cls_inputs,
            err
#if defined(BUZHIDAO_HAVE_OPENCV)
            ,
            original_bgr
#endif
        )) {
        free_pipeline_output(&output);
        return {};
    }
    std::vector<std::pair<int, float>> cls_results;
    if (!run_cls_batches_into(engine, cls_inputs, &cls_ms, &cls_results, err)) {
        free_pipeline_output(&output);
        return {};
    }
    if (!build_rec_candidates(
            &cls_inputs,
            cls_results,
            debug_trace,
            &rotate_ms,
            &rotated_count,
            &rec_candidates,
            err)) {
        free_pipeline_output(&output);
        return {};
    }

    const auto rec_order = build_rec_order(rec_candidates);
    if (debug_trace) {
        dump_rec_candidates_if_requested(debug_dump_dir(), img, rec_candidates, rec_order);
    }
    log_rec_order(rec_candidates, rec_order);
    const auto rec_batches = plan_rec_batches(rec_candidates, rec_order, engine->rec_cfg);
    log_rec_batches(rec_candidates, rec_order, rec_batches, engine->rec_cfg);

    std::vector<std::pair<std::string, float>> rec_results(rec_candidates.size(), {"", 0.0f});
    if (!run_rec_batches_into(
            engine,
            rec_candidates,
            rec_order,
            rec_batches,
            need_rec_debug_meta,
            &rec_results,
            &rec_ms,
            err)) {
        free_pipeline_output(&output);
        return {};
    }

    const auto post_started = std::chrono::steady_clock::now();
    if (!append_pipeline_results(&output, rec_candidates, rec_results, score_thresh, debug_trace, err)) {
        free_pipeline_output(&output);
        return {};
    }
    if (profile_stages) {
        post_ms += elapsed_ms_since(post_started);
        std::ostringstream os;
        os << "run_pipeline profile image=" << image_label
           << ", boxes=" << boxes.size()
           << ", cls_inputs=" << cls_inputs.size()
           << ", rec_candidates=" << rec_candidates.size()
           << ", rotated=" << rotated_count
           << ", load_ms=" << std::fixed << std::setprecision(3) << load_ms
           << ", det_ms=" << det_ms
           << ", crop_ms=" << crop_ms
           << ", cls_ms=" << cls_ms
           << ", rotate_ms=" << rotate_ms
           << ", rec_ms=" << rec_ms
           << ", post_ms=" << post_ms
           << ", total_ms=" << elapsed_ms_since(pipeline_started);
        profile_log(os.str());
    }
    debug_log_lazy([&]() {
        return "run_pipeline: final_detections=" + std::to_string(output.detection_count) +
               ", requested_threshold=" + std::to_string(score_thresh);
    });
    return output;
}

PipelineOutput run_pipeline_from_path(
    buzhi_ocr_engine* engine,
    const fs::path& image_path,
    int det_resize_long,
    float score_thresh,
    int debug_trace,
    char** err
) {
    const bool profile_stages = profile_stages_enabled();
    double load_ms = 0.0;

    std::string load_err;
    const auto load_started = std::chrono::steady_clock::now();
    Image img;
    if (!load_image_file(image_path, img, &load_err)) {
        set_pipeline_error(err, load_err);
        return {};
    }
    if (profile_stages) {
        load_ms += elapsed_ms_since(load_started);
    }
    if (profile_stages) {
        std::ostringstream os;
        os << image_path.filename().string() << " load_ms=" << std::fixed << std::setprecision(3)
           << load_ms;
        debug_log(os.str());
    }
    return run_pipeline(engine, img, image_path.filename().string(), det_resize_long, score_thresh, debug_trace, err);
}
#endif  // BUZHIDAO_HAVE_PADDLE_INFERENCE
