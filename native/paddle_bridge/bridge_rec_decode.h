#ifndef BUZHIDAO_PADDLE_BRIDGE_REC_DECODE_H
#define BUZHIDAO_PADDLE_BRIDGE_REC_DECODE_H

#include <string>
#include <utility>
#include <vector>

std::pair<std::string, float> decode_ctc(
    const float* pred,
    int time_steps,
    int num_classes,
    const std::vector<std::string>& dict
);
std::pair<std::string, float> decode_ctc(
    const std::vector<float>& pred,
    int time_steps,
    int num_classes,
    const std::vector<std::string>& dict
);

#endif
