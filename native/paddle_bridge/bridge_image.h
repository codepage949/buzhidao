#ifndef BUZHIDAO_PADDLE_BRIDGE_IMAGE_H
#define BUZHIDAO_PADDLE_BRIDGE_IMAGE_H

#include "bridge_types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#if __has_include(<opencv2/opencv.hpp>)
#include <opencv2/opencv.hpp>
#define BUZHIDAO_HAVE_OPENCV 1
#endif

uint8_t image_blue_at(const Image& image, size_t idx);
uint8_t image_green_at(const Image& image, size_t idx);
uint8_t image_red_at(const Image& image, size_t idx);

Image resize_bilinear(const Image& img, int target_w, int target_h);
Image resize_for_det(
    const Image& img,
    int det_limit_side_len,
    const std::string& det_limit_type,
    int det_max_side_limit,
    int* resized_h,
    int* resized_w
);
Image make_solid_bgra_image(int width, int height, uint8_t b, uint8_t g, uint8_t r, uint8_t a);
Image pad_rec_input_image(const Image& resized, int tensor_w);
void fill_rect_bgra(
    Image* image,
    int left,
    int top,
    int right,
    int bottom,
    uint8_t b,
    uint8_t g,
    uint8_t r,
    uint8_t a
);
Image make_warmup_pattern_image(int width, int height);

#if defined(BUZHIDAO_HAVE_OPENCV)
cv::Mat image_to_cv_mat_bgra(const Image& image);
cv::Mat image_to_cv_mat_bgr(const Image& image);
Image cv_mat_to_image_bgra(const cv::Mat& input);
#endif

bool load_bmp(const std::filesystem::path& path, Image& out_image, std::string* error);
bool save_bmp(const std::filesystem::path& path, const Image& image, std::string* error);
bool load_image_file(const std::filesystem::path& path, Image& out_image, std::string* error);
Image load_oriented_image(const std::filesystem::path& path, std::string* err);

#endif
