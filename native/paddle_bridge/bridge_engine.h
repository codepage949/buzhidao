#ifndef BUZHIDAO_PADDLE_BRIDGE_ENGINE_H
#define BUZHIDAO_PADDLE_BRIDGE_ENGINE_H

#include "bridge_config.h"
#include "bridge_types.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
#include <paddle_inference_api.h>
#endif

struct buzhi_ocr_engine {
    int use_gpu;
    std::string model_dir;
    std::filesystem::path rec_model_dir;
    ModelPreprocessCfg det_cfg;
    ModelPreprocessCfg cls_cfg;
    ModelPreprocessCfg rec_cfg;
    std::vector<std::string> rec_dict;
    DetOptions det_options;
#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
    paddle_infer::Config det_config;
    paddle_infer::Config cls_config;
    paddle_infer::Config rec_config;
    std::shared_ptr<paddle_infer::Predictor> det_predictor;
    std::shared_ptr<paddle_infer::Predictor> cls_predictor;
    std::shared_ptr<paddle_infer::Predictor> rec_predictor;
    DetScratch det_scratch;
    ClsBatchScratch cls_batch_scratch;
    RecBatchScratch rec_batch_scratch;
#endif
};

#endif
