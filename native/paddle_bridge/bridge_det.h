#ifndef BUZHIDAO_PADDLE_BRIDGE_DET_H
#define BUZHIDAO_PADDLE_BRIDGE_DET_H

#include "bridge_config.h"
#include "bridge_types.h"

#include <memory>
#include <string>
#include <vector>

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
#include <paddle_inference_api.h>

std::vector<BBox> run_det(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const Image& img,
    int det_resize_long,
    const ModelPreprocessCfg& det_cfg,
    const NormalizeCfg& det_norm,
    const DetOptions& det_options,
    std::string* err
);

#endif

#endif
