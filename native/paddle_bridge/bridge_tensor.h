#ifndef BUZHIDAO_PADDLE_BRIDGE_TENSOR_H
#define BUZHIDAO_PADDLE_BRIDGE_TENSOR_H

#include "bridge_config.h"
#include "bridge_types.h"

#include <vector>

void fill_cls_tensor(const Image& resized, const NormalizeCfg& norm, float* dst);
void fill_rec_tensor(const Image& resized, const NormalizeCfg& norm, int tensor_w, float* dst);
void fill_det_tensor(const Image& resized, const NormalizeCfg& norm, float* dst);
bool preprocess_det_into_buffer(
    const Image& img,
    int det_limit_side_len,
    const std::string& det_limit_type,
    int det_max_side_limit,
    const NormalizeCfg& norm,
    FloatScratchBuffer* buffer,
    int* out_h,
    int* out_w,
    float** out_data,
    size_t* out_len,
    std::string* err
);
std::vector<float> preprocess_det(
    const Image& img,
    int det_limit_side_len,
    const std::string& det_limit_type,
    int det_max_side_limit,
    const NormalizeCfg& norm,
    int* out_h,
    int* out_w
);
std::vector<float> preprocess_cls(const Image& img, int target_w, int target_h, const NormalizeCfg& norm);
Image resize_rec_input_image(const Image& img, int target_h, int target_w, int max_w, int& out_w);
std::vector<float> preprocess_rec(
    const Image& img,
    int target_h,
    int target_w,
    int max_w,
    const NormalizeCfg& norm,
    int& out_w
);

#endif
