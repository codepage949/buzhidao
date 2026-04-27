#ifndef BUZHIDAO_PADDLE_BRIDGE_TYPES_H
#define BUZHIDAO_PADDLE_BRIDGE_TYPES_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

struct FloatPoint {
    float x;
    float y;
};

struct OrientedRect {
    FloatPoint center;
    float width;
    float height;
    float angle;
};

struct MinAreaRectBox {
    std::array<FloatPoint, 4> corners;
    OrientedRect rect;
};

enum class PixelLayout {
    BGRA,
    RGBA,
};

struct Image {
    int width;
    int height;
    int channels;
    std::vector<uint8_t> pixels; // layout order + alpha
    PixelLayout layout{PixelLayout::BGRA};
};

struct FloatScratchBuffer {
    std::unique_ptr<float[]> data;
    size_t capacity{0};

    bool ensure(size_t len, std::string* err) {
        if (len <= capacity) {
            return true;
        }
        try {
            data.reset(new float[len]);
            capacity = len;
            return true;
        } catch (const std::bad_alloc&) {
            if (err != nullptr && err->empty()) {
                *err = "OCR scratch buffer 할당 실패";
            }
            return false;
        }
    }

    float* get() { return data.get(); }
    const float* get() const { return data.get(); }
};

struct RecBatchScratch {
    FloatScratchBuffer input;
    FloatScratchBuffer output;
    std::vector<int> output_shape;
};

struct PredictorIoNames {
    std::string input_name;
    std::string output_name;
};

struct BBox {
    std::array<FloatPoint, 4> pts;
    float score;
};

struct RecDebugMeta {
    size_t original_index;
    std::array<FloatPoint, 4> pts;
    std::array<FloatPoint, 4> crop_quad;
    float ratio;
    int cls_label;
    float cls_score;
    bool rotated_180;
    int crop_width;
    int crop_height;
};

struct RecCandidate {
    std::array<FloatPoint, 4> pts;
    std::array<FloatPoint, 4> crop_quad;
    Image cropped;
    float ratio;
    int cls_label;
    float cls_score;
    bool rotated_180;
};

struct ClsPreparedInput {
    Image cropped;
    std::array<FloatPoint, 4> pts;
    std::array<FloatPoint, 4> crop_quad;
};

struct DetDebugCandidate {
    int component_size;
    int component_min_x;
    int component_min_y;
    int component_max_x;
    int component_max_y;
    std::vector<std::pair<int, int>> component_pixels;
    std::vector<float> component_pred;
    std::vector<uint8_t> component_bitmap;
    std::vector<FloatPoint> contour;
    FloatPoint rect_center;
    float rect_width;
    float rect_height;
    float rect_angle;
    std::array<FloatPoint, 4> rect_points;
    std::array<FloatPoint, 4> rect;
    std::array<FloatPoint, 4> unclipped;
    std::array<FloatPoint, 4> scaled;
    int score_x0;
    int score_y0;
    int score_x1;
    int score_y1;
    int score_mask_pixels;
    double score_sum;
    float score;
    bool accepted;
    std::string reject_reason;
};

struct ScoreBoxDebug {
    int x0;
    int y0;
    int x1;
    int y1;
    int mask_pixels;
    double sum;
};

#endif
