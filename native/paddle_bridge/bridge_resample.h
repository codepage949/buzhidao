#ifndef BUZHIDAO_PADDLE_BRIDGE_RESAMPLE_H
#define BUZHIDAO_PADDLE_BRIDGE_RESAMPLE_H

#include "bridge_types.h"

#include <cstdint>

float cubic_weight(float x);
uint8_t sample_channel_cubic_replicate(const Image& img, float sx, float sy, int channel);
uint8_t sample_channel_bilinear_replicate(const Image& img, float sx, float sy, int channel);

#endif
