#ifndef BUZHIDAO_PADDLE_BRIDGE_CLS_H
#define BUZHIDAO_PADDLE_BRIDGE_CLS_H

#include "bridge_config.h"
#include "bridge_types.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
#include <paddle_inference_api.h>

std::pair<int, float> run_cls(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const Image& img,
    const ModelPreprocessCfg& cls_cfg,
    std::string* err
);

std::vector<std::pair<int, float>> run_cls_batch(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const std::vector<const Image*>& imgs,
    const ModelPreprocessCfg& cls_cfg,
    std::string* err
);

#endif

#endif
