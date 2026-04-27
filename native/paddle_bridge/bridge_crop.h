#ifndef BUZHIDAO_PADDLE_BRIDGE_CROP_H
#define BUZHIDAO_PADDLE_BRIDGE_CROP_H

#include "bridge_types.h"

#include <array>
#include <string>

#if __has_include(<opencv2/opencv.hpp>)
#include <opencv2/opencv.hpp>
#define BUZHIDAO_HAVE_OPENCV 1
#endif

#if defined(BUZHIDAO_HAVE_OPENCV)
Image crop_to_bbox(const cv::Mat& img_bgr, const std::array<FloatPoint, 4>& pts, std::string* err);
#endif
Image crop_to_bbox(const Image& img, const std::array<FloatPoint, 4>& pts, std::string* err);

#endif
