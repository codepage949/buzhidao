#ifndef BUZHIDAO_PADDLE_BRIDGE_DET_UTILS_H
#define BUZHIDAO_PADDLE_BRIDGE_DET_UTILS_H

#include "bridge_config.h"
#include "bridge_types.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

void sort_quad_boxes_like_sidecar(std::vector<BBox>* boxes);
void ensure_probability_map(std::vector<float>& map);
void ensure_probability_map(float* map, size_t len);
std::vector<std::vector<int>> neighbors4();
std::vector<std::vector<int>> neighbors8();
void log_det_map_stats(const std::string& prefix, const std::vector<float>& pred, int h, int w);
void log_det_map_stats(const std::string& prefix, const float* pred, size_t len, int h, int w);
std::vector<BBox> db_postprocess(
    const std::vector<float>& pred,
    int pred_h,
    int pred_w,
    int src_h,
    int src_w,
    const DetOptions& options
);
std::vector<BBox> db_postprocess(
    const float* pred,
    size_t pred_len,
    int pred_h,
    int pred_w,
    int src_h,
    int src_w,
    const DetOptions& options
);
bool is_component_cell(
    const std::vector<uint8_t>& component_mask,
    int component_w,
    int component_h,
    int x,
    int y
);
std::vector<FloatPoint> trace_component_contour(
    const std::vector<std::pair<int, int>>& component,
    int pred_w,
    int pred_h
);
float score_box(
    const std::vector<float>& pred,
    int pred_h,
    int pred_w,
    const std::array<FloatPoint, 4>& box,
    ScoreBoxDebug* debug = nullptr
);
float score_box(
    const float* pred,
    size_t pred_len,
    int pred_h,
    int pred_w,
    const std::array<FloatPoint, 4>& box,
    ScoreBoxDebug* debug = nullptr
);

#endif
