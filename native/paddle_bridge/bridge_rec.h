#ifndef BUZHIDAO_PADDLE_BRIDGE_REC_H
#define BUZHIDAO_PADDLE_BRIDGE_REC_H

#include "bridge_config.h"
#include "bridge_types.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
#include <paddle_inference_api.h>

std::pair<std::string, float> run_rec(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const Image& img,
    const std::vector<std::string>& dict,
    const ModelPreprocessCfg& rec_cfg,
    std::string* err
);

std::vector<std::pair<std::string, float>> run_rec_batch(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const std::vector<const Image*>& imgs,
    const std::vector<std::string>& dict,
    const ModelPreprocessCfg& rec_cfg,
    const std::vector<RecDebugMeta>* debug_meta,
    RecBatchScratch* scratch,
    std::string* err
);

#endif

#endif
