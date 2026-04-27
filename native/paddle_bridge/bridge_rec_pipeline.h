#ifndef BUZHIDAO_PADDLE_BRIDGE_REC_PIPELINE_H
#define BUZHIDAO_PADDLE_BRIDGE_REC_PIPELINE_H

#include "bridge_config.h"
#include "bridge_types.h"

#include <cstddef>
#include <utility>
#include <vector>

int estimate_rec_input_width(
    int image_w,
    int image_h,
    int target_h,
    int target_w,
    int max_w
);

std::vector<size_t> build_rec_order(const std::vector<RecCandidate>& rec_candidates);
void log_rec_order(const std::vector<RecCandidate>& rec_candidates, const std::vector<size_t>& rec_order);

std::vector<std::pair<size_t, size_t>> plan_rec_batches(
    const std::vector<RecCandidate>& rec_candidates,
    const std::vector<size_t>& rec_order,
    const ModelPreprocessCfg& rec_cfg
);

void log_rec_batches(
    const std::vector<RecCandidate>& rec_candidates,
    const std::vector<size_t>& rec_order,
    const std::vector<std::pair<size_t, size_t>>& rec_batches,
    const ModelPreprocessCfg& rec_cfg
);

#endif
