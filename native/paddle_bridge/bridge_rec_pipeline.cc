#include "bridge_rec_pipeline.h"

#include "bridge_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>

int estimate_rec_input_width(
    int image_w,
    int image_h,
    int target_h,
    int target_w,
    int max_w
) {
    const int rec_h = std::max(1, target_h);
    const int rec_base_w = std::max(1, target_w);
    const int rec_max_w = std::max(rec_base_w, max_w);
    const float ratio = static_cast<float>(std::max(1, image_w)) / std::max(1, image_h);
    const float base_ratio = static_cast<float>(rec_base_w) / rec_h;
    const float max_wh_ratio = std::max(base_ratio, ratio);
    int dynamic_w = static_cast<int>(rec_h * max_wh_ratio);
    dynamic_w = std::max(rec_base_w, dynamic_w);
    dynamic_w = std::min(rec_max_w, dynamic_w);
    return dynamic_w;
}

std::vector<size_t> build_rec_order(const std::vector<RecCandidate>& rec_candidates) {
    std::vector<size_t> rec_order(rec_candidates.size());
    for (size_t i = 0; i < rec_order.size(); ++i) {
        rec_order[i] = i;
    }
    std::stable_sort(rec_order.begin(), rec_order.end(), [&](size_t lhs, size_t rhs) {
        return rec_candidates[lhs].ratio < rec_candidates[rhs].ratio;
    });
    return rec_order;
}

void log_rec_order(const std::vector<RecCandidate>& rec_candidates, const std::vector<size_t>& rec_order) {
    if (!debug_enabled()) {
        return;
    }
    std::ostringstream os;
    os << "run_pipeline rec_order=";
    for (size_t i = 0; i < rec_order.size(); ++i) {
        if (i > 0) {
            os << " | ";
        }
        const auto& candidate = rec_candidates[rec_order[i]];
        os << "#" << rec_order[i]
           << ":" << candidate.cropped.width << "x" << candidate.cropped.height
           << "@r=" << std::fixed << std::setprecision(4) << candidate.ratio;
    }
    debug_log(os.str());
}

std::vector<std::pair<size_t, size_t>> plan_rec_batches(
    const std::vector<RecCandidate>& rec_candidates,
    const std::vector<size_t>& rec_order,
    const ModelPreprocessCfg& rec_cfg
) {
    std::vector<std::pair<size_t, size_t>> rec_batches;
    rec_batches.reserve((rec_order.size() + 5) / 6);
    constexpr size_t kRecBatchSize = 6;
    int width_budget = 0;
    parse_env_int(std::getenv("BUZHIDAO_PADDLE_FFI_REC_BATCH_WIDTH_BUDGET"), &width_budget);
    size_t batch_start = 0;
    while (batch_start < rec_order.size()) {
        if (width_budget > 0) {
            size_t batch_end = batch_start;
            int batch_max_w = 0;
            while (batch_end < rec_order.size() && batch_end - batch_start < kRecBatchSize) {
                const auto& candidate = rec_candidates[rec_order[batch_end]];
                const int estimated_w = estimate_rec_input_width(
                    candidate.cropped.width,
                    candidate.cropped.height,
                    rec_cfg.rec_target_h,
                    rec_cfg.rec_target_w,
                    rec_cfg.rec_max_w
                );
                const int next_max_w = std::max(batch_max_w, estimated_w);
                const size_t next_count = batch_end - batch_start + 1;
                if (batch_end > batch_start &&
                    next_max_w * static_cast<int>(next_count) > width_budget) {
                    break;
                }
                batch_max_w = next_max_w;
                ++batch_end;
            }
            rec_batches.push_back({batch_start, batch_end});
            batch_start = batch_end;
            continue;
        }
        const size_t batch_end = std::min(batch_start + kRecBatchSize, rec_order.size());
        rec_batches.push_back({batch_start, batch_end});
        batch_start = batch_end;
    }
    return rec_batches;
}

void log_rec_batches(
    const std::vector<RecCandidate>& rec_candidates,
    const std::vector<size_t>& rec_order,
    const std::vector<std::pair<size_t, size_t>>& rec_batches,
    const ModelPreprocessCfg& rec_cfg
) {
    if (!debug_enabled()) {
        return;
    }
    std::ostringstream os;
    os << "run_pipeline rec_batches=";
    for (size_t i = 0; i < rec_batches.size(); ++i) {
        if (i > 0) {
            os << " | ";
        }
        const auto [start, end] = rec_batches[i];
        int batch_max_w = 0;
        for (size_t pos = start; pos < end; ++pos) {
            const auto& candidate = rec_candidates[rec_order[pos]];
            batch_max_w = std::max(
                batch_max_w,
                estimate_rec_input_width(
                    candidate.cropped.width,
                    candidate.cropped.height,
                    rec_cfg.rec_target_h,
                    rec_cfg.rec_target_w,
                    rec_cfg.rec_max_w
                )
            );
        }
        os << "#" << i << ":" << (end - start) << "@" << batch_max_w;
    }
    debug_log(os.str());
}
