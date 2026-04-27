#ifndef BUZHIDAO_PADDLE_BRIDGE_CONFIG_H
#define BUZHIDAO_PADDLE_BRIDGE_CONFIG_H

#include <array>
#include <filesystem>
#include <string>
#include <vector>

constexpr float DET_THRESH = 0.3f;
constexpr float DET_BOX_THRESH = 0.6f;
constexpr float DET_UNCLIP = 1.5f;
constexpr float DET_MIN_SIDE = 3.0f;
constexpr int DET_RESIZE_LONG = 960;
constexpr int DET_LIMIT_SIDE_LEN = 64;
constexpr int DET_MAX_SIDE_LIMIT = 4000;
constexpr float DET_MAX_RATIO_W = 1000.0f;
constexpr float DET_MAX_RATIO_H = 1000.0f;
constexpr float BGR_MEAN[3] = {0.485f, 0.456f, 0.406f};
constexpr float BGR_STD[3] = {0.229f, 0.224f, 0.225f};
constexpr int DET_ALIGN = 32;
constexpr int REC_H = 48;
constexpr int REC_MAX_W = 3200;
constexpr int CLS_W = 160;
constexpr int CLS_H = 80;

struct DetOptions {
    float threshold;
    float box_threshold;
    float min_side;
    float unclip_ratio;
    int max_candidates;
};

struct NormalizeCfg {
    std::array<float, 3> mean{{BGR_MEAN[0], BGR_MEAN[1], BGR_MEAN[2]}};
    std::array<float, 3> std{{BGR_STD[0], BGR_STD[1], BGR_STD[2]}};
    float scale{1.0f / 255.0f};
};

struct ModelPreprocessCfg {
    int resize_long{960};
    int det_limit_side_len{DET_LIMIT_SIDE_LEN};
    int det_max_side_limit{DET_MAX_SIDE_LIMIT};
    std::string det_limit_type{"min"};
    int rec_target_h{REC_H};
    int rec_target_w{320};
    int rec_max_w{REC_MAX_W};
    int cls_target_w{CLS_W};
    int cls_target_h{CLS_H};
    int det_max_candidates{1000};
    float det_threshold{DET_THRESH};
    float det_box_threshold{DET_BOX_THRESH};
    float det_min_side{DET_MIN_SIDE};
    float det_unclip_ratio{DET_UNCLIP};
    NormalizeCfg det_norm{};
    NormalizeCfg cls_norm{};
    NormalizeCfg rec_norm{{0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, 1.0f / 255.0f};
    std::vector<std::string> rec_dict;
};

ModelPreprocessCfg load_model_preprocess_cfg(const std::filesystem::path& model_dir);
DetOptions resolve_det_options(const ModelPreprocessCfg& model_cfg);

#endif
