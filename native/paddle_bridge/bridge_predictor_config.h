#ifndef BUZHIDAO_PADDLE_BRIDGE_PREDICTOR_CONFIG_H
#define BUZHIDAO_PADDLE_BRIDGE_PREDICTOR_CONFIG_H

#include <filesystem>
#include <utility>

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
#include <paddle_inference_api.h>

bool predictor_flag_enabled(
    const char* global_env_name,
    const char* stage_env_name,
    bool default_value
);
void configure_predictor(
    paddle_infer::Config* config,
    const std::pair<std::filesystem::path, std::filesystem::path>& model_pair,
    bool use_gpu,
    const char* predictor_kind
);
#endif

#endif
