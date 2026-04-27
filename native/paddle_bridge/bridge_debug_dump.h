#ifndef BUZHIDAO_PADDLE_BRIDGE_DEBUG_DUMP_H
#define BUZHIDAO_PADDLE_BRIDGE_DEBUG_DUMP_H

#include "bridge_types.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

void dump_crop_stage_if_enabled(
    const char* tag,
    const std::array<FloatPoint, 4>& input_pts,
    const std::array<FloatPoint, 4>& quad,
    int out_w,
    int out_h,
    const Image& image
);

void dump_rec_candidates_if_requested(
    const std::string& dump_dir,
    const Image& img,
    const std::vector<RecCandidate>& rec_candidates,
    const std::vector<size_t>& rec_order
);

void dump_candidate_crop_if_requested(const RecCandidate& candidate, const char* tag, const Image& image);

#endif
