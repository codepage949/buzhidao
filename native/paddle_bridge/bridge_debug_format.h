#ifndef BUZHIDAO_PADDLE_BRIDGE_DEBUG_FORMAT_H
#define BUZHIDAO_PADDLE_BRIDGE_DEBUG_FORMAT_H

#include "bridge_types.h"

#include <array>
#include <string>
#include <vector>

std::string quote_polygon(const std::array<FloatPoint, 4>& pts);
std::string quote_points(const std::vector<FloatPoint>& pts);

#endif
