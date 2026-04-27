#ifndef BUZHIDAO_PADDLE_BRIDGE_PREDICTOR_H
#define BUZHIDAO_PADDLE_BRIDGE_PREDICTOR_H

#include "bridge_types.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
#include <paddle_inference_api.h>

bool run_predictor(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const std::vector<float>& input,
    const std::vector<int>& shape,
    std::vector<float>& output,
    std::vector<int>& output_shape,
    std::string* err
);

bool run_predictor_into_buffer(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const float* input_data,
    size_t input_len,
    const std::vector<int>& shape,
    FloatScratchBuffer* output,
    size_t* output_len,
    std::vector<int>& output_shape,
    std::string* err
);
#endif

int find_rec_layout(const std::vector<int>& shape, int& time_steps, int& num_classes);

#endif
