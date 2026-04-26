#include "bridge.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <cstdlib>
#include <cerrno>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <filesystem>
#if !defined(_WIN32)
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

#if __has_include(<clipper.hpp>)
#include <clipper.hpp>
#define BUZHIDAO_HAVE_PYCLIPPER_CLIPPER 1
#endif

#if __has_include(<opencv2/opencv.hpp>)
#include <opencv2/opencv.hpp>
#define BUZHIDAO_HAVE_OPENCV 1
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#endif

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
#include <paddle_inference_api.h>
#endif

constexpr float DET_THRESH = 0.3f;
constexpr float DET_BOX_THRESH = 0.6f;
constexpr float DET_UNCLIP = 1.5f;
constexpr float DET_MIN_SIDE = 3.0f;
constexpr int DET_RESIZE_LONG = 960;
constexpr int DET_LIMIT_SIDE_LEN = 64;
constexpr int DET_MAX_SIDE_LIMIT = 4000;
constexpr float DET_MAX_RATIO_W = 1000.0f;
constexpr float DET_MAX_RATIO_H = 1000.0f;
constexpr float BGR_MEAN[3] = {0.485f, 0.456f, 0.406f};
constexpr float BGR_STD[3] = {0.229f, 0.224f, 0.225f};
constexpr int DET_ALIGN = 32;
constexpr int REC_H = 48;
constexpr int REC_MAX_W = 3200;
constexpr int CLS_W = 160;
constexpr int CLS_H = 80;
constexpr char RAW_DICT_HINT[] =
    "다음 중 하나로 사전이 있어야 합니다: rec_dict.txt, ppocr_keys_v1.txt, ppocr_keys_v2.txt.";

struct DetOptions {
    float threshold;
    float box_threshold;
    float min_side;
    float unclip_ratio;
    int max_candidates;
};

struct NormalizeCfg {
    std::array<float, 3> mean{{BGR_MEAN[0], BGR_MEAN[1], BGR_MEAN[2]}};
    std::array<float, 3> std{{BGR_STD[0], BGR_STD[1], BGR_STD[2]}};
    float scale{1.0f / 255.0f};
};

struct ModelPreprocessCfg {
    int resize_long{960};
    int det_limit_side_len{DET_LIMIT_SIDE_LEN};
    int det_max_side_limit{DET_MAX_SIDE_LIMIT};
    std::string det_limit_type{"min"};
    int rec_target_h{REC_H};
    int rec_target_w{320};
    int rec_max_w{REC_MAX_W};
    int cls_target_w{CLS_W};
    int cls_target_h{CLS_H};
    int det_max_candidates{1000};
    float det_threshold{DET_THRESH};
    float det_box_threshold{DET_BOX_THRESH};
    float det_min_side{DET_MIN_SIDE};
    float det_unclip_ratio{DET_UNCLIP};
    NormalizeCfg det_norm{};
    NormalizeCfg cls_norm{};
    NormalizeCfg rec_norm{{0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, 1.0f / 255.0f};
    std::vector<std::string> rec_dict;
};

struct buzhi_ocr_engine {
    int use_gpu;
    std::string model_dir;
    fs::path rec_model_dir;
    ModelPreprocessCfg det_cfg;
    ModelPreprocessCfg cls_cfg;
    ModelPreprocessCfg rec_cfg;
    std::vector<std::string> rec_dict;
    DetOptions det_options;
#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
    paddle_infer::Config det_config;
    paddle_infer::Config cls_config;
    paddle_infer::Config rec_config;
    std::shared_ptr<paddle_infer::Predictor> det_predictor;
    std::shared_ptr<paddle_infer::Predictor> cls_predictor;
    std::shared_ptr<paddle_infer::Predictor> rec_predictor;
#endif
};

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

inline uint8_t image_blue_at(const Image& image, size_t idx) {
    return image.layout == PixelLayout::RGBA ? image.pixels[idx + 2] : image.pixels[idx + 0];
}

inline uint8_t image_green_at(const Image& image, size_t idx) {
    return image.pixels[idx + 1];
}

inline uint8_t image_red_at(const Image& image, size_t idx) {
    return image.layout == PixelLayout::RGBA ? image.pixels[idx + 0] : image.pixels[idx + 2];
}

Image make_solid_bgra_image(int width, int height, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    Image image{
        std::max(1, width),
        std::max(1, height),
        4,
        std::vector<uint8_t>(static_cast<size_t>(std::max(1, width) * std::max(1, height)) * 4u, 0)
    };
    for (size_t i = 0; i < image.pixels.size(); i += 4) {
        image.pixels[i + 0] = b;
        image.pixels[i + 1] = g;
        image.pixels[i + 2] = r;
        image.pixels[i + 3] = a;
    }
    return image;
}

void fill_rect_bgra(Image* image, int left, int top, int right, int bottom, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    if (image == nullptr || image->width <= 0 || image->height <= 0) {
        return;
    }
    const int clamped_left = std::max(0, std::min(left, image->width));
    const int clamped_top = std::max(0, std::min(top, image->height));
    const int clamped_right = std::max(clamped_left, std::min(right, image->width));
    const int clamped_bottom = std::max(clamped_top, std::min(bottom, image->height));
    for (int y = clamped_top; y < clamped_bottom; ++y) {
        for (int x = clamped_left; x < clamped_right; ++x) {
            const size_t idx = static_cast<size_t>(y * image->width + x) * 4u;
            image->pixels[idx + 0] = b;
            image->pixels[idx + 1] = g;
            image->pixels[idx + 2] = r;
            image->pixels[idx + 3] = a;
        }
    }
}

Image make_warmup_pattern_image(int width, int height) {
    Image image = make_solid_bgra_image(width, height, 255, 255, 255, 255);
    const int safe_width = std::max(1, image.width);
    const int safe_height = std::max(1, image.height);
    const int margin_x = std::max(4, safe_width / 24);
    const int margin_y = std::max(4, safe_height / 16);
    const int line_h = std::max(6, safe_height / 8);
    const int glyph_w = std::max(8, safe_width / 18);
    const int glyph_gap = std::max(4, glyph_w / 3);
    for (int row = 0; row < 3; ++row) {
        const int top = margin_y + row * (line_h + margin_y);
        if (top >= safe_height) {
            break;
        }
        const int bottom = std::min(safe_height, top + line_h);
        int cursor_x = margin_x;
        for (int col = 0; col < 5 && cursor_x < safe_width - margin_x; ++col) {
            const int bar_count = 2 + ((row + col) % 3);
            for (int bar = 0; bar < bar_count; ++bar) {
                const int bar_left = cursor_x + bar * (glyph_w / std::max(1, bar_count));
                const int bar_right = std::min(safe_width, bar_left + std::max(3, glyph_w / std::max(2, bar_count)));
                const int inset_top = top + ((bar % 2) ? line_h / 6 : line_h / 12);
                const int inset_bottom = std::max(inset_top + 2, bottom - ((bar % 2) ? line_h / 12 : line_h / 6));
                fill_rect_bgra(&image, bar_left, inset_top, bar_right, inset_bottom, 16, 16, 16, 255);
            }
            cursor_x += glyph_w + glyph_gap;
        }
    }
    return image;
}

#if defined(BUZHIDAO_HAVE_OPENCV)
cv::Mat image_to_cv_mat_bgra(const Image& image) {
    if (image.width <= 0 || image.height <= 0 || image.pixels.empty()) {
        return {};
    }
    cv::Mat view(
        image.height,
        image.width,
        CV_8UC4,
        const_cast<uint8_t*>(image.pixels.data())
    );
    if (image.layout == PixelLayout::BGRA) {
        return view.clone();
    }
    cv::Mat bgra;
    cv::cvtColor(view, bgra, cv::COLOR_RGBA2BGRA);
    return bgra;
}

cv::Mat image_to_cv_mat_bgr(const Image& image) {
    if (image.width <= 0 || image.height <= 0 || image.pixels.empty()) {
        return {};
    }
    cv::Mat view(
        image.height,
        image.width,
        CV_8UC4,
        const_cast<uint8_t*>(image.pixels.data())
    );
    cv::Mat bgr;
    if (image.layout == PixelLayout::RGBA) {
        cv::cvtColor(view, bgr, cv::COLOR_RGBA2BGR);
    } else {
        cv::cvtColor(view, bgr, cv::COLOR_BGRA2BGR);
    }
    if (bgr.empty()) {
        return {};
    }
    return bgr;
}

Image cv_mat_to_image_bgra(const cv::Mat& input) {
    if (input.empty()) {
        return {0, 0, 0, {}};
    }
    cv::Mat bgra;
    if (input.channels() == 4) {
        bgra = input;
    } else if (input.channels() == 3) {
        cv::cvtColor(input, bgra, cv::COLOR_BGR2BGRA);
    } else if (input.channels() == 1) {
        cv::cvtColor(input, bgra, cv::COLOR_GRAY2BGRA);
    } else {
        return {0, 0, 0, {}};
    }
    Image out{
        bgra.cols,
        bgra.rows,
        4,
        std::vector<uint8_t>(static_cast<size_t>(bgra.total() * bgra.elemSize()))
    };
    std::memcpy(out.pixels.data(), bgra.data, out.pixels.size());
    return out;
}
#endif

struct BBox {
    std::array<FloatPoint, 4> pts;
    float score;
};

void sort_quad_boxes_like_sidecar(std::vector<BBox>* boxes) {
    if (boxes == nullptr || boxes->size() <= 1) {
        return;
    }
    auto& items = *boxes;
    std::stable_sort(items.begin(), items.end(), [](const BBox& lhs, const BBox& rhs) {
        const auto& a = lhs.pts[0];
        const auto& b = rhs.pts[0];
        if (std::fabs(a.y - b.y) > 1e-4f) {
            return a.y < b.y;
        }
        return a.x < b.x;
    });
    for (size_t i = 0; i + 1 < items.size(); ++i) {
        for (size_t j = i + 1; j > 0; --j) {
            const auto& curr = items[j];
            const auto& prev = items[j - 1];
            if (std::fabs(curr.pts[0].y - prev.pts[0].y) < 10.0f &&
                curr.pts[0].x < prev.pts[0].x) {
                std::swap(items[j], items[j - 1]);
                continue;
            }
            break;
        }
    }
}

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

uint8_t sample_channel_bilinear_replicate(const Image& img, float sx, float sy, int channel);

std::string normalize_hint(std::string value);
std::string to_lower_ascii(std::string value);
bool file_exists(const fs::path& path);
bool directory_exists(const fs::path& path);
std::vector<fs::path> list_direct_child_dirs(const fs::path& root);
bool has_stem_files_in_dir(const fs::path& dir);
std::string quote_polygon(const std::array<FloatPoint, 4>& pts);
std::string quote_points(const std::vector<FloatPoint>& pts);
bool debug_enabled();
void debug_log(const std::string& message);
void profile_log(const std::string& message);
bool parse_env_float(const char* value, float* out);
bool parse_env_int(const char* value, int* out);
std::unordered_set<std::string> parse_env_csv_set(const char* value);
std::string read_text_file(const fs::path& path);
std::string trim_or_empty(const std::string& value);
float parse_scale_value(const std::string& value, float fallback);
std::string json_escape(const std::string& text);
std::string lookup_json_key(
    const std::string& text,
    const std::vector<std::string>& keys,
    bool preserve_string = false
);
std::string extract_json_key_value(
    const std::string& text,
    const std::string& key,
    bool preserve_string = false
);
std::string parse_json_string(const std::string& text, size_t& i);
std::string extract_json_block(
    const std::string& text,
    size_t start
);
std::vector<std::string> parse_json_array(
    const std::string& array_text
);
bool parse_float_list(
    const std::string& text,
    const std::string& key,
    std::array<float, 3>& out
);
bool parse_int_list(
    const std::string& text,
    const std::string& key,
    std::array<int, 3>& out
);
bool parse_float(
    const std::string& token,
    float* out
);
bool parse_int(
    const std::string& token,
    int* out
);
ModelPreprocessCfg load_model_preprocess_cfg(const fs::path& model_dir);
DetOptions resolve_det_options(const ModelPreprocessCfg& model_cfg);
bool load_image_file(const fs::path& path, Image& out_image, std::string* error);
void dump_crop_stage_if_enabled(
    const char* tag,
    const std::array<FloatPoint, 4>& input_pts,
    const std::array<FloatPoint, 4>& quad,
    int out_w,
    int out_h,
    const Image& image
);

char* dup_string(const std::string& value) {
    char* out = new (std::nothrow) char[value.size() + 1];
    if (out == nullptr) {
        return nullptr;
    }
    std::memcpy(out, value.c_str(), value.size() + 1);
    return out;
}

void set_error(char** err, const std::string& message) {
    if (err != nullptr) {
        *err = dup_string(message);
    }
}

void set_error_if_empty(std::string* err, const std::string& message) {
    if (err != nullptr && err->empty()) {
        *err = message;
    }
}

bool debug_enabled() {
    const char* raw = std::getenv("BUZHIDAO_PADDLE_FFI_TRACE");
    if (raw == nullptr || raw[0] == '\0') {
        return false;
    }
    const auto token = to_lower_ascii(normalize_hint(raw));
    return token == "1" || token == "true" || token == "on" || token == "yes";
}

bool profile_stages_enabled() {
    const char* raw = std::getenv("BUZHIDAO_PADDLE_FFI_PROFILE_STAGES");
    if (raw == nullptr || raw[0] == '\0') {
        return false;
    }
    const auto token = to_lower_ascii(normalize_hint(raw));
    return token == "1" || token == "true" || token == "on" || token == "yes";
}

double elapsed_ms_since(const std::chrono::steady_clock::time_point& started) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started
    ).count();
}

std::string debug_dump_dir() {
    const char* raw = std::getenv("BUZHIDAO_PADDLE_FFI_DUMP_DIR");
    if (raw == nullptr || raw[0] == '\0') {
        return {};
    }
    const std::string normalized = normalize_hint(raw);
    std::error_code ec;
    fs::create_directories(fs::path(normalized), ec);
    if (ec) {
        std::cerr << "[buzhi_ocr] dump dir create failed: " << normalized
                  << ", error=" << ec.message() << std::endl;
        return {};
    }
    return normalized;
}

void debug_log(const std::string& message) {
    if (!debug_enabled()) {
        return;
    }
    std::cerr << "[buzhi_ocr] " << message << std::endl;
    const char* tmp_dir = std::getenv("TMPDIR");
    const std::string log_path =
        std::string((tmp_dir != nullptr && tmp_dir[0] != '\0') ? tmp_dir : "/tmp") +
        "/buzhi-ocr-ffi-debug.log";
    std::ofstream log_file(log_path, std::ios::app);
    if (log_file) {
        log_file << "[buzhi_ocr] " << message << std::endl;
    }
}

void profile_log(const std::string& message) {
    if (!profile_stages_enabled()) {
        return;
    }
    std::cerr << "[buzhi_ocr_profile] " << message << std::endl;
    const char* tmp_dir = std::getenv("TMPDIR");
    const std::string log_path =
        std::string((tmp_dir != nullptr && tmp_dir[0] != '\0') ? tmp_dir : "/tmp") +
        "/buzhi-ocr-ffi-profile.log";
    std::ofstream log_file(log_path, std::ios::app);
    if (log_file) {
        log_file << "[buzhi_ocr_profile] " << message << std::endl;
    }
}

template <typename Builder>
void debug_log_lazy(Builder&& builder) {
    if (!debug_enabled()) {
        return;
    }
    debug_log(builder());
}

std::string trim(std::string value) {
    auto left = std::find_if_not(value.begin(), value.end(), [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    });
    auto right = std::find_if_not(value.rbegin(), value.rend(), [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    }).base();
    if (left >= right) {
        return {};
    }
    return std::string(left, right);
}

std::string normalize_hint(std::string value) {
    auto normalized = trim(std::move(value));
    if (normalized.empty()) {
        return {};
    }
    normalized = to_lower_ascii(std::move(normalized));
    return normalized;
}

bool parse_env_float(const char* value, float* out) {
    if (value == nullptr || out == nullptr) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(value, &end);
    if (errno != 0 || end == value || *end != '\0') {
        return false;
    }
    *out = parsed;
    return true;
}

bool parse_env_int(const char* value, int* out) {
    if (value == nullptr || out == nullptr) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return false;
    }
    *out = static_cast<int>(parsed);
    return true;
}

std::unordered_set<std::string> parse_env_csv_set(const char* value) {
    std::unordered_set<std::string> out;
    if (value == nullptr) {
        return out;
    }
    std::string current;
    auto flush = [&out, &current]() {
        if (current.empty()) {
            return;
        }
        size_t start = 0;
        while (start < current.size() && std::isspace(static_cast<unsigned char>(current[start])) != 0) {
            ++start;
        }
        size_t end = current.size();
        while (end > start && std::isspace(static_cast<unsigned char>(current[end - 1])) != 0) {
            --end;
        }
        if (start < end) {
            out.insert(current.substr(start, end - start));
        }
        current.clear();
    };
    for (const char* p = value; *p != '\0'; ++p) {
        if (*p == ',') {
            flush();
            continue;
        }
        current.push_back(*p);
    }
    flush();
    return out;
}

bool parse_env_bool(const char* value, bool* out) {
    if (value == nullptr || out == nullptr) {
        return false;
    }
    const auto normalized = to_lower_ascii(trim(value));
    if (
        normalized == "1" ||
        normalized == "true" ||
        normalized == "yes" ||
        normalized == "on" ||
        normalized == "y"
    ) {
        *out = true;
        return true;
    }
    if (
        normalized == "0" ||
        normalized == "false" ||
        normalized == "no" ||
        normalized == "off" ||
        normalized == "n"
    ) {
        *out = false;
        return true;
    }
    return false;
}

std::string trim_or_empty(const std::string& value) {
    return trim(value);
}

void append_utf8_codepoint(std::string& out, uint32_t codepoint) {
    if (codepoint <= 0x7Fu) {
        out.push_back(static_cast<char>(codepoint));
        return;
    }
    if (codepoint <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | ((codepoint >> 6) & 0x1Fu)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
        return;
    }
    if (codepoint <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | ((codepoint >> 12) & 0x0Fu)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
        return;
    }
    out.push_back(static_cast<char>(0xF0u | ((codepoint >> 18) & 0x07u)));
    out.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
}

bool parse_hex4(const std::string& text, size_t pos, uint32_t& value) {
    if (pos + 4 > text.size()) {
        return false;
    }
    value = 0;
    for (size_t j = 0; j < 4; ++j) {
        const char ch = text[pos + j];
        value <<= 4;
        if (ch >= '0' && ch <= '9') {
            value |= static_cast<uint32_t>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            value |= static_cast<uint32_t>(10 + ch - 'a');
        } else if (ch >= 'A' && ch <= 'F') {
            value |= static_cast<uint32_t>(10 + ch - 'A');
        } else {
            return false;
        }
    }
    return true;
}

std::string parse_json_string(const std::string& text, size_t& i) {
    if (i >= text.size() || text[i] != '\"') {
        return {};
    }
    ++i;
    std::string out;
    for (; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '\\') {
            ++i;
            if (i >= text.size()) {
                return {};
            }
            const char esc = text[i];
            switch (esc) {
            case '\"':
            case '\\':
            case '/':
                out.push_back(esc);
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                uint32_t codepoint = 0;
                if (!parse_hex4(text, i + 1, codepoint)) {
                    return {};
                }
                i += 4;
                if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
                    if (i + 6 >= text.size() || text[i + 1] != '\\' || text[i + 2] != 'u') {
                        return {};
                    }
                    uint32_t low = 0;
                    if (!parse_hex4(text, i + 3, low) || low < 0xDC00u || low > 0xDFFFu) {
                        return {};
                    }
                    i += 6;
                    codepoint = 0x10000u + (((codepoint - 0xD800u) << 10) | (low - 0xDC00u));
                }
                append_utf8_codepoint(out, codepoint);
                break;
            }
            default:
                out.push_back(esc);
                break;
            }
            continue;
        }
        if (ch == '\"') {
            ++i;
            return out;
        }
        out.push_back(ch);
    }
    return {};
}

std::string extract_json_block(const std::string& text, size_t start) {
    if (start >= text.size()) {
        return {};
    }
    const char open = text[start];
    const char close = open == '{' ? '}' : ']';
    if (open != '{' && open != '[') {
        return {};
    }
    int depth = 0;
    bool in_str = false;
    bool escape = false;
    for (size_t i = start; i < text.size(); ++i) {
        const char ch = text[i];
        if (in_str) {
            if (escape) {
                escape = false;
                continue;
            }
            if (ch == '\\') {
                escape = true;
                continue;
            }
            if (ch == '\"') {
                in_str = false;
            }
            continue;
        }
        if (ch == '\"') {
            in_str = true;
            continue;
        }
        if (ch == open) {
            ++depth;
            continue;
        }
        if (ch == close) {
            --depth;
            if (depth == 0) {
                return text.substr(start, i - start + 1);
            }
        }
    }
    return {};
}

std::string extract_json_key_value(
    const std::string& text,
    const std::string& key,
    bool preserve_string
) {
    const std::string quoted_key = "\"" + key + "\"";
    size_t pos = 0;
    while (true) {
        pos = text.find(quoted_key, pos);
        if (pos == std::string::npos) {
            return {};
        }

        const size_t colon = text.find(':', pos + quoted_key.size());
        if (colon == std::string::npos) {
            return {};
        }

        size_t i = colon + 1;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
            ++i;
        }
        if (i >= text.size()) {
            return {};
        }

        if (text[i] == '{' || text[i] == '[') {
            return extract_json_block(text, i);
        }
        if (text[i] == '\"') {
            const size_t start = i;
            auto parsed = parse_json_string(text, i);
            return preserve_string ? parsed : text.substr(start, i - start);
        }

        size_t end = i;
        bool in_str = false;
        bool esc = false;
        int depth_obj = 0;
        int depth_arr = 0;
        while (end < text.size()) {
            const char ch = text[end];
            if (in_str) {
                if (esc) {
                    esc = false;
                } else if (ch == '\\') {
                    esc = true;
                } else if (ch == '\"') {
                    in_str = false;
                }
            } else if (ch == '\"') {
                in_str = true;
            } else if (ch == '{') {
                ++depth_obj;
            } else if (ch == '}') {
                if (depth_obj > 0) {
                    --depth_obj;
                }
            } else if (ch == '[') {
                ++depth_arr;
            } else if (ch == ']') {
                if (depth_arr > 0) {
                    --depth_arr;
                }
            } else if (ch == ',' && depth_obj == 0 && depth_arr == 0) {
                break;
            }
            ++end;
        }
        return trim_or_empty(text.substr(i, end - i));
    }
}

std::vector<std::string> parse_json_array(const std::string& array_text) {
    const auto trimmed = trim_or_empty(array_text);
    if (trimmed.size() < 2 || trimmed.front() != '[' || trimmed.back() != ']') {
        return {};
    }
    std::vector<std::string> values;
    size_t i = 1;
    while (i + 1 < trimmed.size()) {
        while (i < trimmed.size() - 1 && (trimmed[i] == ',' || std::isspace(static_cast<unsigned char>(trimmed[i])) != 0)) {
            ++i;
        }
        if (i + 1 >= trimmed.size()) {
            break;
        }

        if (trimmed[i] == '\"') {
            const size_t start = i;
            const auto item = parse_json_string(trimmed, i);
            values.push_back(item);
            while (i < trimmed.size() && trimmed[i] != ',' && trimmed[i] != ']') {
                ++i;
            }
            continue;
        }

        if (trimmed[i] == '{' || trimmed[i] == '[') {
            const auto block = extract_json_block(trimmed, i);
            if (block.empty()) {
                return {};
            }
            values.push_back(block);
            i += block.size();
            continue;
        }

        const size_t start = i;
        bool in_str = false;
        bool esc = false;
        int depth_obj = 0;
        int depth_arr = 0;
        while (i < trimmed.size() - 1) {
            const char ch = trimmed[i];
            if (in_str) {
                if (esc) {
                    esc = false;
                    ++i;
                    continue;
                }
                if (ch == '\\') {
                    esc = true;
                    ++i;
                    continue;
                }
                if (ch == '\"') {
                    in_str = false;
                }
                ++i;
                continue;
            }
            if (ch == '\"') {
                in_str = true;
                ++i;
                continue;
            }
            if (ch == '{') {
                ++depth_obj;
            } else if (ch == '}') {
                if (depth_obj > 0) {
                    --depth_obj;
                }
            } else if (ch == '[') {
                ++depth_arr;
            } else if (ch == ']') {
                if (depth_arr > 0) {
                    --depth_arr;
                }
            } else if (ch == ',' && depth_obj == 0 && depth_arr == 0) {
                break;
            }
            ++i;
        }
        values.push_back(trim_or_empty(trimmed.substr(start, i - start)));
        if (i < trimmed.size() && trimmed[i] != ',') {
            ++i;
        }
    }
    return values;
}

bool parse_float_list(const std::string& text, const std::string& key, std::array<float, 3>& out) {
    const auto raw = extract_json_key_value(text, key);
    const auto values = parse_json_array(raw);
    if (values.size() < 3) {
        return false;
    }
    for (size_t i = 0; i < 3; ++i) {
        if (!parse_float(values[i], &out[i])) {
            return false;
        }
    }
    return true;
}

bool parse_int_list(const std::string& text, const std::string& key, std::array<int, 3>& out) {
    const auto raw = extract_json_key_value(text, key);
    const auto values = parse_json_array(raw);
    if (values.size() < 3) {
        return false;
    }
    for (size_t i = 0; i < 3; ++i) {
        if (!parse_int(values[i], &out[i])) {
            return false;
        }
    }
    return true;
}

float parse_scale_value(const std::string& value, float fallback) {
    const auto trimmed = trim_or_empty(value);
    float parsed = fallback;
    if (parse_float(trimmed, &parsed)) {
        return parsed;
    }
    const auto slash = trimmed.find('/');
    if (slash != std::string::npos) {
        float left = 0.0f;
        float right = 1.0f;
        if (parse_float(trimmed.substr(0, slash), &left) &&
            parse_float(trimmed.substr(slash + 1), &right) &&
            right != 0.0f) {
            return left / right;
        }
    }
    return fallback;
}

bool parse_float(const std::string& token, float* out) {
    if (out == nullptr) {
        return false;
    }
    const auto t = trim_or_empty(token);
    if (t.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(t.c_str(), &end);
    if (errno != 0 || end == t.c_str() || *end != '\0') {
        return false;
    }
    *out = parsed;
    return true;
}

bool parse_int(const std::string& token, int* out) {
    if (out == nullptr) {
        return false;
    }
    const auto t = trim_or_empty(token);
    if (t.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(t.c_str(), &end, 10);
    if (errno != 0 || end == t.c_str() || *end != '\0') {
        return false;
    }
    *out = static_cast<int>(parsed);
    return true;
}

std::string read_text_file(const fs::path& path) {
    std::ifstream in(path);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string lookup_json_key(
    const std::string& text,
    const std::vector<std::string>& keys,
    bool preserve_string
) {
    for (const auto& key : keys) {
        const auto value = extract_json_key_value(text, key, preserve_string);
        if (!value.empty()) {
            return value;
        }
    }
    return {};
}

ModelPreprocessCfg load_model_preprocess_cfg(const fs::path& model_dir) {
    ModelPreprocessCfg cfg{};
    if (model_dir.empty()) {
        return cfg;
    }

    const auto model_name = to_lower_ascii(model_dir.filename().string());
    const bool is_det = model_name.find("det") != std::string::npos;
    const bool is_cls = model_name.find("cls") != std::string::npos ||
                        model_name.find("angle") != std::string::npos;
    const bool is_rec = model_name.find("rec") != std::string::npos || (!is_det && !is_cls);

    const std::vector<fs::path> config_paths = {
        model_dir / "config.json",
        model_dir / "config.yaml",
        model_dir / "config.yml",
        model_dir / "inference.json",
        model_dir / "inference_config.json",
        model_dir / "inference.yaml",
        model_dir / "inference.yml",
    };

    std::string config_text;
    for (const auto& path : config_paths) {
        const auto text = read_text_file(path);
        if (!text.empty()) {
            config_text = std::move(text);
            debug_log(std::string("load_model_preprocess_cfg loaded: ") + path.string());
            break;
        }
    }
    if (config_text.empty()) {
        return cfg;
    }

    const auto pick = [&](const std::string& source, const std::vector<const char*>& keys, bool preserve_string = false) {
        for (const auto* key : keys) {
            const auto value = extract_json_key_value(source, key, preserve_string);
            if (!value.empty()) {
                return value;
            }
        }
        return std::string{};
    };

    const auto parse_int_field = [&](const std::string& source, const std::vector<const char*>& keys, int& out) {
        for (const auto* key : keys) {
            const auto raw = pick(source, {key});
            if (raw.empty()) {
                continue;
            }
            float f = 0.0f;
            if (parse_float(raw, &f) && f > 0.0f) {
                out = static_cast<int>(std::llround(f));
                return true;
            }
            int v = 0;
            if (parse_int(raw, &v) && v > 0) {
                out = v;
                return true;
            }
        }
        return false;
    };

    const auto parse_float_field = [&](const std::string& source, const std::vector<const char*>& keys, float& out) {
        for (const auto* key : keys) {
            const auto raw = pick(source, {key});
            if (raw.empty()) {
                continue;
            }
            if (parse_float(raw, &out)) {
                return true;
            }
        }
        return false;
    };

    const auto parse_wh = [&](const std::string& source, const std::vector<const char*>& keys, int& w, int& h) {
        for (const auto* key : keys) {
            const auto raw = pick(source, {key});
            if (raw.empty()) {
                continue;
            }
            const auto values = parse_json_array(raw);
            if (values.size() >= 2) {
                int v0 = 0;
                int v1 = 0;
                if (parse_int(values[0], &v0) && parse_int(values[1], &v1) && v0 > 0 && v1 > 0) {
                    w = v0;
                    h = v1;
                    return true;
                }
                float f0 = 0.0f;
                float f1 = 0.0f;
                if (parse_float(values[0], &f0) && parse_float(values[1], &f1)) {
                    w = std::max(1, static_cast<int>(std::llround(f0)));
                    h = std::max(1, static_cast<int>(std::llround(f1)));
                    return true;
                }
            }
        }
        return false;
    };

    const auto parse_hw = [&](const std::string& source, const std::vector<const char*>& keys, int& h, int& w) {
        for (const auto* key : keys) {
            const auto raw = pick(source, {key});
            if (raw.empty()) {
                continue;
            }
            const auto values = parse_json_array(raw);
            if (values.size() >= 3) {
                int a = 0;
                int b = 0;
                int c = 0;
                if (parse_int(values[0], &a) && parse_int(values[1], &b) && parse_int(values[2], &c)) {
                    if (a == 3 && b > 0 && c > 0) {
                        h = b;
                        w = c;
                        return true;
                    }
                    if (c == 3 && a > 0 && b > 0) {
                        h = a;
                        w = b;
                        return true;
                    }
                    h = b;
                    w = c;
                    return true;
                }
            }
            if (values.size() == 2) {
                int v0 = 0;
                int v1 = 0;
                if (parse_int(values[0], &v0) && parse_int(values[1], &v1) && v0 > 0 && v1 > 0) {
                    h = v0;
                    w = v1;
                    return true;
                }
            }
        }
        return false;
    };

    const auto parse_normalize = [&](const std::string& source, NormalizeCfg& target) {
        std::array<float, 3> mean = target.mean;
        if (parse_float_list(source, "mean", mean)) {
            target.mean = mean;
        }
        std::array<float, 3> stdv = target.std;
        if (parse_float_list(source, "std", stdv)) {
            target.std = stdv;
        }
        const auto scale_raw = pick(source, {"scale"});
        if (!scale_raw.empty()) {
            target.scale = parse_scale_value(scale_raw, target.scale);
        }
    };

    const auto read_lines = [](const fs::path& path) {
        std::vector<std::string> lines;
        std::ifstream in(path);
        if (!in) {
            return lines;
        }
        std::string line;
        while (std::getline(in, line)) {
            const auto trimmed = trim(line);
            if (!trimmed.empty()) {
                lines.push_back(trimmed);
            }
        }
        return lines;
    };

    const auto parse_dict = [&](const std::string& source, std::vector<std::string>& out_dict) {
        const auto raw = trim_or_empty(pick(source, {"character_dict", "character_dict_path", "dict_path"}, true));
        if (raw.empty()) {
            return;
        }
        if (raw.size() >= 2 && raw.front() == '[' && raw.back() == ']') {
            const auto values = parse_json_array(raw);
            if (!values.empty()) {
                out_dict = values;
            }
        } else {
            fs::path dict_path = raw;
            if (!dict_path.is_absolute()) {
                dict_path = model_dir / dict_path;
            }
            const auto lines = read_lines(dict_path);
            if (!lines.empty()) {
                out_dict = lines;
            }
        }
        if (!out_dict.empty() &&
            std::find(out_dict.begin(), out_dict.end(), std::string(" ")) == out_dict.end()) {
            out_dict.push_back(" ");
        }
    };

    const auto parse_dynamic_rec_width = [&](const std::string& source, int& target_w) {
        if (source.empty()) {
            return;
        }
        const auto x_shapes = trim_or_empty(pick(source, {"x"}, false));
        if (x_shapes.empty()) {
            return;
        }
        const auto shapes = parse_json_array(x_shapes);
        int max_w = target_w;
        for (const auto& shape_raw : shapes) {
            const auto dims = parse_json_array(shape_raw);
            if (dims.size() < 4) {
                continue;
            }
            int w = 0;
            if (parse_int(dims[3], &w) && w > 0) {
                max_w = std::max(max_w, w);
            }
        }
        target_w = max_w;
    };

    const auto preprocess = trim_or_empty(pick(config_text, {"PreProcess", "preprocess"}, false));
    const auto postprocess = trim_or_empty(pick(config_text, {"PostProcess", "postprocess"}, false));
    const auto hpi = trim_or_empty(pick(config_text, {"Hpi", "hpi"}, false));
    const auto backend_configs = trim_or_empty(pick(hpi, {"backend_configs"}, false));
    const auto paddle_infer_cfg = trim_or_empty(pick(backend_configs, {"paddle_infer"}, false));
    const auto trt_dynamic_shapes = trim_or_empty(pick(paddle_infer_cfg, {"trt_dynamic_shapes"}, false));
    const auto tensorrt_cfg = trim_or_empty(pick(backend_configs, {"tensorrt"}, false));
    const auto dynamic_shapes = trim_or_empty(pick(tensorrt_cfg, {"dynamic_shapes"}, false));
    const auto transform_ops_text = trim_or_empty(
        !preprocess.empty() ? pick(preprocess, {"transform_ops", "transforms"}, false)
        : pick(config_text, {"transform_ops", "transforms"}, false)
    );
    const auto transform_ops = parse_json_array(transform_ops_text);

    for (const auto& op_raw : transform_ops) {
        if (op_raw.empty()) {
            continue;
        }
        const auto det_resize_cfg = trim_or_empty(lookup_json_key(op_raw, {"DetResizeForTest", "ResizeForTest"}, false));
        const auto rec_resize_cfg = trim_or_empty(lookup_json_key(op_raw, {"RecResizeImg", "ResizeImage"}, false));
        const auto cls_resize_cfg = trim_or_empty(
            lookup_json_key(op_raw, {"ResizeImage", "CropImage"}, false)
        );
        const auto norm_cfg = trim_or_empty(lookup_json_key(op_raw, {"NormalizeImage"}, false));
        const auto op_name = to_lower_ascii(trim_or_empty(lookup_json_key(op_raw, {"type", "op", "name"}, true)));

        if (is_det && (!det_resize_cfg.empty() || op_name.find("detresize") != std::string::npos)) {
            const auto source = det_resize_cfg.empty() ? op_raw : det_resize_cfg;
            parse_int_field(source, {"resize_long", "limit_side_len", "target_long", "max_side_len"}, cfg.resize_long);
            parse_normalize(source, cfg.det_norm);
        }
        if (is_rec && (!rec_resize_cfg.empty() || op_name.find("recresizeimg") != std::string::npos)) {
            const auto source = rec_resize_cfg.empty() ? op_raw : rec_resize_cfg;
            parse_hw(source, {"image_shape"}, cfg.rec_target_h, cfg.rec_target_w);
            parse_wh(source, {"size"}, cfg.rec_target_w, cfg.rec_target_h);
            parse_normalize(source, cfg.rec_norm);
        }
        if (is_cls && (!cls_resize_cfg.empty() || op_name.find("cropimage") != std::string::npos)) {
            const auto source = cls_resize_cfg.empty() ? op_raw : cls_resize_cfg;
            parse_wh(source, {"size"}, cfg.cls_target_w, cfg.cls_target_h);
            parse_normalize(source, cfg.cls_norm);
        }
        if (!norm_cfg.empty()) {
            if (is_det) {
                parse_normalize(norm_cfg, cfg.det_norm);
            }
            if (is_cls) {
                parse_normalize(norm_cfg, cfg.cls_norm);
            }
            if (is_rec) {
                parse_normalize(norm_cfg, cfg.rec_norm);
            }
        }
    }

    if (is_rec) {
        parse_dynamic_rec_width(trt_dynamic_shapes, cfg.rec_max_w);
        parse_dynamic_rec_width(dynamic_shapes, cfg.rec_max_w);
        cfg.rec_max_w = std::max(cfg.rec_max_w, cfg.rec_target_w);
    }

    if (is_det && !postprocess.empty()) {
        parse_float_field(postprocess, {"thresh", "threshold"}, cfg.det_threshold);
        parse_float_field(postprocess, {"box_thresh", "box_threshold"}, cfg.det_box_threshold);
        parse_float_field(postprocess, {"min_side"}, cfg.det_min_side);
        parse_float_field(postprocess, {"unclip_ratio", "unclip"}, cfg.det_unclip_ratio);
        parse_int_field(postprocess, {"max_candidates", "max_text"}, cfg.det_max_candidates);
    }

    if (is_rec) {
        parse_dict(postprocess, cfg.rec_dict);
    }

    return cfg;
}

DetOptions resolve_det_options(const ModelPreprocessCfg& model_cfg) {
    auto clamp01 = [](float v) {
        return std::clamp(v, 0.0f, 1.0f);
    };
    auto clamp_min = [](float v, float min_value) {
        return v <= 0.0f ? min_value : v;
    };

    DetOptions opts{};
    opts.threshold = clamp01(model_cfg.det_threshold);
    opts.box_threshold = clamp01(model_cfg.det_box_threshold);
    opts.min_side = clamp_min(model_cfg.det_min_side, 0.0f);
    opts.unclip_ratio = clamp_min(model_cfg.det_unclip_ratio, 0.0f);
    opts.max_candidates = std::max(1, model_cfg.det_max_candidates);

    float env_float = 0.0f;
    if (parse_env_float(std::getenv("BUZHIDAO_PADDLE_FFI_DET_THRESH"), &env_float)) {
        opts.threshold = clamp01(env_float);
    }
    if (parse_env_float(std::getenv("BUZHIDAO_PADDLE_FFI_DET_BOX_THRESH"), &env_float)) {
        opts.box_threshold = clamp01(env_float);
    }
    if (parse_env_float(std::getenv("BUZHIDAO_PADDLE_FFI_DET_MIN_SIDE"), &env_float)) {
        opts.min_side = clamp_min(env_float, 0.0f);
    }
    if (parse_env_float(std::getenv("BUZHIDAO_PADDLE_FFI_DET_UNCLIP"), &env_float)) {
        opts.unclip_ratio = clamp_min(env_float, 0.0f);
    }
    int env_candidates = 0;
    if (parse_env_int(std::getenv("BUZHIDAO_PADDLE_FFI_DET_MAX_CANDIDATES"), &env_candidates) &&
        env_candidates > 0) {
        opts.max_candidates = env_candidates;
    }
    return opts;
}

std::string to_lower_ascii(std::string value) {
    for (auto& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

std::vector<std::string> find_stem_aliases(const std::string& stem) {
    if (stem == "det") {
        return {
            "det",
            "textdet",
            "text_det",
            "detection",
            "textdetv",
            "text_detection",
        };
    }
    if (stem == "cls") {
        return {
            "cls",
            "textcls",
            "textline",
            "orientation",
            "angle",
            "textorientation",
        };
    }
    return {
        "rec",
        "textrec",
        "text_rec",
        "recognition",
        "textrecg",
        "text_recog",
    };
}

std::vector<std::string> find_stem_family_suffixes(const std::string& stem) {
    if (stem == "det") {
        return {
            "_textdetv",
            "_text_detection",
            "_textdet",
            "_text_det",
            "_det",
            "_detect",
        };
    }
    if (stem == "cls") {
        return {
            "_textorientation",
            "_textline_ori",
            "_text_line_ori",
            "_textline",
            "_orientation",
            "_angle",
            "_cls",
        };
    }
    return {
        "_textrecog",
        "_text_recog",
        "_textrec",
        "_text_rec",
        "_recognition",
        "_mobile_rec",
        "_rec",
    };
}

std::string resolve_preferred_lang() {
    const char* raw = std::getenv("BUZHIDAO_PADDLE_FFI_SOURCE");
    if (raw == nullptr) {
        return "en";
    }
    const std::string value = normalize_hint(raw);
    if (value.empty()) {
        return "en";
    }
    if (value == "en" || value == "eng" || value == "english") {
        return "en";
    }
    if (
        value == "cn" ||
        value == "zh" ||
        value == "ch" ||
        value == "chi" ||
        value == "chinese" ||
        value.rfind("ch_", 0) == 0 ||
        value.rfind("zh-", 0) == 0 ||
        value.rfind("zh_", 0) == 0
    ) {
        return "ch";
    }
    return value;
}

void ensure_probability_map(std::vector<float>& map) {
    if (map.empty()) {
        return;
    }
    auto minmax = std::minmax_element(map.begin(), map.end());
    const float mn = *minmax.first;
    const float mx = *minmax.second;
    if (mn < 0.0f || mx > 1.0f) {
        for (auto& v : map) {
            v = 1.0f / (1.0f + std::exp(-v));
        }
    }
}

std::string resolve_model_preference() {
    const char* raw = std::getenv("BUZHIDAO_PADDLE_FFI_MODEL_HINT");
    return normalize_hint(raw == nullptr ? "" : raw);
}

std::vector<fs::path> list_named_submodel_dirs(const fs::path& model_root, const std::string& stem) {
    std::vector<fs::path> paths;
    if (model_root.empty()) {
        debug_log("list_named_submodel_dirs: empty model_root, stem=" + stem);
        return paths;
    }
    if (!directory_exists(model_root)) {
        debug_log("list_named_submodel_dirs: invalid model_root=" + model_root.string() + ", stem=" + stem);
        return paths;
    }
    const auto aliases = find_stem_aliases(stem);

    try {
        for (const auto& path : list_direct_child_dirs(model_root)) {
            const std::string name = to_lower_ascii(path.filename().string());
            const bool is_match = std::any_of(
                aliases.begin(),
                aliases.end(),
                [&](const auto& alias) { return name.find(alias) != std::string::npos; }
            );
            if (!is_match) {
                continue;
            }
            paths.push_back(path);
        }
    } catch (const std::exception& ex) {
        debug_log(
            std::string("list_named_submodel_dirs failed: ") + model_root.string() +
            ", stem=" + stem + ", error=" + ex.what()
        );
        return {};
    }

    std::sort(paths.begin(), paths.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename().string() < b.filename().string();
    });
    return paths;
}

std::string infer_model_family_hint(const fs::path& model_path, const std::string& stem) {
    auto name = to_lower_ascii(model_path.filename().string());
    const auto suffixes = find_stem_family_suffixes(stem);
    for (const auto& suffix : suffixes) {
        const auto pos = name.rfind(suffix);
        if (pos != std::string::npos) {
            auto family = name.substr(0, pos);
            while (!family.empty() && (family.back() == '_' || family.back() == '-')) {
                family.pop_back();
            }
            return family;
        }
    }
    return {};
}

bool prefer_model_by_lang(const std::string& candidate, const std::string& pref_source) {
    if (pref_source.empty()) {
        return false;
    }
    const auto lang_in = [&](std::initializer_list<const char*> tokens) {
        for (const auto* token : tokens) {
            if (pref_source == token) {
                return true;
            }
        }
        return false;
    };
    if (pref_source == "ch" || pref_source == "chinese_cht" || pref_source == "japan") {
        return candidate.find("server_rec") != std::string::npos;
    }
    if (pref_source == "en") {
        return candidate.find("en_") != std::string::npos;
    }
    if (pref_source == "korean") {
        return candidate.find("korean_") != std::string::npos;
    }
    if (pref_source == "th") {
        return candidate.find("th_") != std::string::npos;
    }
    if (pref_source == "el") {
        return candidate.find("el_") != std::string::npos;
    }
    if (pref_source == "te") {
        return candidate.find("te_") != std::string::npos;
    }
    if (pref_source == "ta") {
        return candidate.find("ta_") != std::string::npos;
    }
    if (lang_in({
            "af", "az", "bs", "cs", "cy", "da", "de", "es", "et", "fr", "ga", "hr", "hu", "id",
            "is", "it", "ku", "la", "lt", "lv", "mi", "ms", "mt", "nl", "no", "oc", "pi", "pl",
            "pt", "ro", "rs_latin", "sk", "sl", "sq", "sv", "sw", "tl", "tr", "uz", "vi",
            "french", "german", "fi", "eu", "gl", "lb", "rm", "ca", "qu"
        })) {
        return candidate.find("latin_") != std::string::npos;
    }
    if (pref_source == "ru" || pref_source == "be" || pref_source == "uk") {
        return candidate.find("eslav_") != std::string::npos;
    }
    if (lang_in({
            "rs_cyrillic", "bg", "mn", "abq", "ady", "kbd", "ava", "dar", "inh", "che", "lbe",
            "lez", "tab", "kk", "ky", "tg", "mk", "tt", "cv", "ba", "mhr", "mo", "udm", "kv",
            "os", "bua", "xal", "tyv", "sah", "kaa"
        })) {
        return candidate.find("cyrillic_") != std::string::npos;
    }
    if (lang_in({"ar", "fa", "ug", "ur", "ps", "sd", "bal"})) {
        return candidate.find("arabic_") != std::string::npos;
    }
    if (lang_in({"hi", "mr", "ne", "bh", "mai", "ang", "bho", "mah", "sck", "new", "gom", "sa", "bgc"})) {
        return candidate.find("devanagari_") != std::string::npos;
    }
    if (candidate == pref_source) {
        return true;
    }
    const std::string tokens[] = {
        "_" + pref_source + "_",
        "-" + pref_source + "-",
        "_" + pref_source,
        "-" + pref_source,
        pref_source + "_",
        pref_source + "-",
    };
    for (const auto& token : tokens) {
        if (candidate.find(token) != std::string::npos) {
            return true;
        }
    }
    return candidate.find(pref_source) != std::string::npos;
}

bool is_textline_orientation_model(const std::string& name) {
    return name.find("textline") != std::string::npos || name.find("text_line") != std::string::npos;
}

std::pair<fs::path, fs::path> resolve_candidate_model_pair(
    const fs::path& model_dir,
    const std::string& stem,
    const std::string& preferred_token,
    const std::string& preferred_lang,
    const std::string& preferred_family
) {
    debug_log(std::string("resolve_candidate_model_pair stem=") + stem +
              ", preferred_token=" + preferred_token +
              ", preferred_lang=" + preferred_lang +
              ", preferred_family=" + preferred_family +
              ", root=" + model_dir.string());
    const auto direct_json = model_dir / (stem + ".json");
    const auto direct_pdiparams = model_dir / (stem + ".pdiparams");
    const auto direct_pdmodel = model_dir / (stem + ".pdmodel");

    if (file_exists(direct_json) && file_exists(direct_pdiparams)) {
        return {direct_json, direct_pdiparams};
    }
    if (file_exists(direct_pdmodel) && file_exists(direct_pdiparams)) {
        return {direct_pdmodel, direct_pdiparams};
    }

    const fs::path direct_dir = model_dir / stem;
    std::vector<fs::path> candidates;
    if (directory_exists(direct_dir) && has_stem_files_in_dir(direct_dir)) {
        candidates.push_back(direct_dir);
    }

    auto named_dirs = list_named_submodel_dirs(model_dir, stem);
    candidates.insert(candidates.end(), named_dirs.begin(), named_dirs.end());

    for (const auto& path : candidates) {
        if (!has_stem_files_in_dir(path)) {
            continue;
        }
        const auto name = to_lower_ascii(path.filename().string());
        if (!preferred_token.empty() && name.find(preferred_token) != std::string::npos) {
            debug_log("candidate matched token: " + name);
            const auto preferred_json = path / "inference.json";
            const auto preferred_params = path / "inference.pdiparams";
            const auto preferred_model = path / "inference.pdmodel";
            if (file_exists(preferred_json) && file_exists(preferred_params)) {
                debug_log("candidate selected by token: " + name + ", " + preferred_json.string());
                return {preferred_json, preferred_params};
            }
            if (file_exists(preferred_model) && file_exists(preferred_params)) {
                debug_log("candidate selected by token: " + name + ", " + preferred_model.string());
                return {preferred_model, preferred_params};
            }
        }
    }

    for (const auto& path : candidates) {
        if (!has_stem_files_in_dir(path)) {
            continue;
        }
        const auto name = to_lower_ascii(path.filename().string());
        if (prefer_model_by_lang(name, preferred_lang)) {
            const auto model_json = path / "inference.json";
            const auto model_params = path / "inference.pdiparams";
            const auto model_model = path / "inference.pdmodel";
            if (file_exists(model_json) && file_exists(model_params)) {
                debug_log("candidate selected by lang: " + name + ", " + model_json.string());
                return {model_json, model_params};
            }
            if (file_exists(model_model) && file_exists(model_params)) {
                debug_log("candidate selected by lang: " + name + ", " + model_model.string());
                return {model_model, model_params};
            }
        }
    }

    if (!preferred_family.empty()) {
        for (const auto& path : candidates) {
            if (!has_stem_files_in_dir(path)) {
                continue;
            }
            const auto name = to_lower_ascii(path.filename().string());
            if (name.find(preferred_family) != std::string::npos) {
                const auto family_json = path / "inference.json";
                const auto family_params = path / "inference.pdiparams";
                const auto family_model = path / "inference.pdmodel";
                if (file_exists(family_json) && file_exists(family_params)) {
                    debug_log("candidate selected by family: " + name + ", " + family_json.string());
                    return {family_json, family_params};
                }
                if (file_exists(family_model) && file_exists(family_params)) {
                    debug_log("candidate selected by family: " + name + ", " + family_model.string());
                    return {family_model, family_params};
                }
            }
        }
    }

    if (stem == "cls") {
        for (const auto& path : candidates) {
            if (!has_stem_files_in_dir(path)) {
                continue;
            }
            const auto name = to_lower_ascii(path.filename().string());
            if (!is_textline_orientation_model(name)) {
                continue;
            }
            const auto preferred_json = path / "inference.json";
            const auto preferred_params = path / "inference.pdiparams";
            const auto preferred_model = path / "inference.pdmodel";
            if (file_exists(preferred_json) && file_exists(preferred_params)) {
                debug_log("candidate selected by cls textline preference: " + name + ", " + preferred_json.string());
                return {preferred_json, preferred_params};
            }
            if (file_exists(preferred_model) && file_exists(preferred_params)) {
                debug_log("candidate selected by cls textline preference: " + name + ", " + preferred_model.string());
                return {preferred_model, preferred_params};
            }
        }
    }

    for (const auto& path : candidates) {
        if (!has_stem_files_in_dir(path)) {
            continue;
        }
        const auto name = to_lower_ascii(path.filename().string());
        const auto model_json = path / "inference.json";
        const auto model_params = path / "inference.pdiparams";
        const auto model_model = path / "inference.pdmodel";
        if (file_exists(model_json) && file_exists(model_params)) {
            debug_log("candidate fallback: " + name + ", " + model_json.string());
            return {model_json, model_params};
        }
        if (file_exists(model_model) && file_exists(model_params)) {
            debug_log("candidate fallback: " + name + ", " + model_model.string());
            return {model_model, model_params};
        }
    }
    debug_log("candidate not found: " + stem + ", root=" + model_dir.string());
    return {"", ""};
}

bool is_json_like_dict_name(const std::string& name) {
    const std::string lower = to_lower_ascii(name);
    return lower.find("dict") != std::string::npos ||
        lower.find("character") != std::string::npos ||
        lower.find("key") != std::string::npos;
}

bool file_exists(const fs::path& path) {
    if (path.empty()) {
        return false;
    }
#if defined(_WIN32)
    try {
        return fs::exists(path) && fs::is_regular_file(path);
    } catch (const std::exception& ex) {
        debug_log(std::string("file_exists failed: ") + path.string() + ", error=" + ex.what());
        return false;
    }
#else
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool directory_exists(const fs::path& path) {
    if (path.empty()) {
        return false;
    }
#if defined(_WIN32)
    try {
        return fs::exists(path) && fs::is_directory(path);
    } catch (const std::exception& ex) {
        debug_log(std::string("directory_exists failed: ") + path.string() + ", error=" + ex.what());
        return false;
    }
#else
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

std::vector<fs::path> list_direct_child_dirs(const fs::path& root) {
    std::vector<fs::path> dirs;
    if (root.empty()) {
        return dirs;
    }
#if defined(_WIN32)
    try {
        for (const auto& entry : fs::directory_iterator(root)) {
            if (entry.is_directory()) {
                dirs.push_back(entry.path());
            }
        }
    } catch (const std::exception& ex) {
        debug_log(std::string("list_direct_child_dirs failed: ") + root.string() + ", error=" + ex.what());
        return {};
    }
#else
    DIR* dir = ::opendir(root.c_str());
    if (dir == nullptr) {
        return dirs;
    }
    while (dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        const fs::path child = root / name;
        if (directory_exists(child)) {
            dirs.push_back(child);
        }
    }
    ::closedir(dir);
#endif
    return dirs;
}

std::vector<uint8_t> read_all_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }
    const auto size = in.tellg();
    if (size <= 0) {
        return {};
    }
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

std::string json_escape(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size() + text.size() / 4);
    for (const auto ch : text) {
        switch (ch) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped.push_back(ch);
        }
    }
    return escaped;
}

template <typename T>
bool read_u8(const std::vector<uint8_t>& bytes, size_t* cursor, T* out) {
    if (*cursor + sizeof(T) > bytes.size()) {
        return false;
    }
    std::memcpy(out, bytes.data() + *cursor, sizeof(T));
    *cursor += sizeof(T);
    return true;
}

bool read_u16_le(const std::vector<uint8_t>& bytes, size_t* cursor, uint16_t* out) {
    return read_u8(bytes, cursor, out);
}

bool read_u32_le(const std::vector<uint8_t>& bytes, size_t* cursor, uint32_t* out) {
    return read_u8(bytes, cursor, out);
}

bool load_bmp(const fs::path& path, Image& out_image, std::string* error) {
    auto bytes = read_all_bytes(path);
    if (bytes.empty()) {
        if (error != nullptr) {
            *error = "이미지 파일을 읽지 못했습니다: " + path.string();
        }
        return false;
    }

    const std::string ext = to_lower_ascii(path.extension().string());
    if (ext != ".bmp" && ext != ".dib") {
        if (error != nullptr) {
            *error = "현재 BMP 이미지만 지원됩니다: " + path.string();
        }
        return false;
    }

    size_t cursor = 0;
    uint16_t signature;
    if (!read_u16_le(bytes, &cursor, &signature)) {
        if (error != nullptr) {
            *error = "BMP 헤더를 읽을 수 없습니다";
        }
        return false;
    }
    if (signature != 0x4D42) {
        if (error != nullptr) {
            *error = "BMP 시그니처가 아닙니다";
        }
        return false;
    }

    cursor = 0x0A;
    uint32_t pixel_offset;
    if (!read_u32_le(bytes, &cursor, &pixel_offset)) {
        if (error != nullptr) {
            *error = "BMP 오프셋을 읽을 수 없습니다";
        }
        return false;
    }

    cursor = 0x0E;
    uint32_t dib_size;
    if (!read_u32_le(bytes, &cursor, &dib_size)) {
        if (error != nullptr) {
            *error = "BMP DIB 헤더를 읽을 수 없습니다";
        }
        return false;
    }
    if (dib_size < 40) {
        if (error != nullptr) {
            *error = "지원되지 않는 BMP DIB 크기";
        }
        return false;
    }

    int32_t width;
    int32_t height;
    cursor = 0x12;
    uint16_t planes;
    uint16_t bits_per_pixel;
    uint32_t compression;
    if (!read_u32_le(bytes, &cursor, reinterpret_cast<uint32_t*>(&width)) ||
        !read_u32_le(bytes, &cursor, reinterpret_cast<uint32_t*>(&height)) ||
        !read_u16_le(bytes, &cursor, &planes) ||
        !read_u16_le(bytes, &cursor, &bits_per_pixel) ||
        !read_u32_le(bytes, &cursor, &compression)) {
        if (error != nullptr) {
            *error = "BMP 기본 헤더 파싱에 실패했습니다";
        }
        return false;
    }
    if (width <= 0 || height == 0) {
        if (error != nullptr) {
            *error = "지원되지 않는 BMP 크기입니다";
        }
        return false;
    }
    if (planes != 1) {
        if (error != nullptr) {
            *error = "BMP 비트맵은 평면 수 1이 필요합니다";
        }
        return false;
    }
    if (compression != 0) {
        if (error != nullptr) {
            *error = "압축된 BMP는 현재 지원되지 않습니다";
        }
        return false;
    }
    if (bits_per_pixel != 24 && bits_per_pixel != 32) {
        if (error != nullptr) {
            *error = "24/32bit BMP만 지원됩니다";
        }
        return false;
    }

    const int channels = bits_per_pixel == 32 ? 4 : 3;
    const int abs_h = std::abs(height);
    const bool top_down = height < 0;
    const int row_stride = (bits_per_pixel * width + 31) / 32 * 4;
    const size_t pixel_data_start = static_cast<size_t>(pixel_offset);
    const size_t expected_size = pixel_data_start + static_cast<size_t>(row_stride) * abs_h;
    if (expected_size > bytes.size()) {
        if (error != nullptr) {
            *error = "BMP 픽셀 데이터가 손상되었습니다";
        }
        return false;
    }

    out_image.width = width;
    out_image.height = abs_h;
    out_image.channels = 4;
    out_image.pixels.assign(static_cast<size_t>(width * abs_h * 4), 0);

    const uint8_t* pixel_base = bytes.data() + pixel_data_start;
    for (int y = 0; y < abs_h; ++y) {
        const int src_y = top_down ? y : (abs_h - 1 - y);
        const uint8_t* row = pixel_base + static_cast<size_t>(src_y) * row_stride;
        for (int x = 0; x < width; ++x) {
            const uint8_t* src = row + static_cast<size_t>(x) * channels;
            const size_t dst_idx = static_cast<size_t>((y * width + x) * 4);
            out_image.pixels[dst_idx + 0] = src[0];
            out_image.pixels[dst_idx + 1] = src[1];
            out_image.pixels[dst_idx + 2] = src[2];
            out_image.pixels[dst_idx + 3] = (channels == 4) ? src[3] : 255u;
        }
    }
    if (debug_enabled()) {
        const size_t sample_offset = 0;
        debug_log(std::string("load_bmp path=") + path.string() +
                  ", width=" + std::to_string(width) + ", height=" + std::to_string(height) +
                  ", abs_h=" + std::to_string(abs_h) + ", channels=" + std::to_string(channels) +
                  ", top_down=" + (top_down ? "true" : "false") +
                  ", row_stride=" + std::to_string(row_stride) +
                  ", bgr0=" + std::to_string(out_image.pixels[sample_offset]) + "," +
                  std::to_string(out_image.pixels[sample_offset + 1]) + "," +
                  std::to_string(out_image.pixels[sample_offset + 2]));
    }
    return true;
}

bool save_bmp(const fs::path& path, const Image& image, std::string* error) {
    if (image.width <= 0 || image.height <= 0 || image.channels != 4) {
        if (error != nullptr) {
            *error = "BMP 저장 대상 이미지 형식이 잘못되었습니다";
        }
        return false;
    }

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        if (error != nullptr) {
            *error = "BMP 파일을 열 수 없습니다: " + path.string();
        }
        return false;
    }

    const uint32_t row_stride = static_cast<uint32_t>((image.width * 3 + 3) & ~3);
    const uint32_t pixel_bytes = row_stride * static_cast<uint32_t>(image.height);
    const uint32_t file_size = 14u + 40u + pixel_bytes;

    uint8_t file_header[14] = {
        'B', 'M',
        0, 0, 0, 0,
        0, 0, 0, 0,
        54, 0, 0, 0,
    };
    file_header[2] = static_cast<uint8_t>(file_size & 0xFFu);
    file_header[3] = static_cast<uint8_t>((file_size >> 8) & 0xFFu);
    file_header[4] = static_cast<uint8_t>((file_size >> 16) & 0xFFu);
    file_header[5] = static_cast<uint8_t>((file_size >> 24) & 0xFFu);

    uint8_t info_header[40] = {};
    info_header[0] = 40;
    const auto write_u32 = [&](int offset, uint32_t value) {
        info_header[offset + 0] = static_cast<uint8_t>(value & 0xFFu);
        info_header[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
        info_header[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
        info_header[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
    };
    write_u32(4, static_cast<uint32_t>(image.width));
    write_u32(8, static_cast<uint32_t>(image.height));
    info_header[12] = 1;
    info_header[14] = 24;
    write_u32(20, pixel_bytes);
    write_u32(24, 2835);
    write_u32(28, 2835);

    file.write(reinterpret_cast<const char*>(file_header), sizeof(file_header));
    file.write(reinterpret_cast<const char*>(info_header), sizeof(info_header));

    std::vector<uint8_t> row(row_stride, 0);
    for (int y = image.height - 1; y >= 0; --y) {
        for (int x = 0; x < image.width; ++x) {
            const size_t src = (static_cast<size_t>(y) * image.width + x) * 4;
            const size_t dst = static_cast<size_t>(x) * 3;
            row[dst + 0] = image.pixels[src + 0];
            row[dst + 1] = image.pixels[src + 1];
            row[dst + 2] = image.pixels[src + 2];
        }
        file.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }

    if (!file) {
        if (error != nullptr) {
            *error = "BMP 파일 저장에 실패했습니다: " + path.string();
        }
        return false;
    }
    return true;
}

#if defined(_WIN32)
bool ensure_gdiplus(std::string* error) {
    static bool initialized = false;
    static ULONG_PTR token = 0;
    if (initialized) {
        return true;
    }
    Gdiplus::GdiplusStartupInput input;
    const auto status = Gdiplus::GdiplusStartup(&token, &input, nullptr);
    if (status != Gdiplus::Ok) {
        if (error != nullptr) {
            *error = "GDI+ 초기화 실패";
        }
        return false;
    }
    initialized = true;
    return true;
}

bool load_bitmap_with_gdiplus(const fs::path& path, Image& out_image, std::string* error) {
    if (!ensure_gdiplus(error)) {
        return false;
    }
    Gdiplus::Bitmap bitmap(path.wstring().c_str());
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        if (error != nullptr) {
            *error = "이미지 파일을 열 수 없습니다: " + path.string();
        }
        return false;
    }
    const UINT w = bitmap.GetWidth();
    const UINT h = bitmap.GetHeight();
    if (w == 0 || h == 0) {
        if (error != nullptr) {
            *error = "이미지 크기가 0입니다: " + path.string();
        }
        return false;
    }

    Gdiplus::Rect rect(0, 0, static_cast<INT>(w), static_cast<INT>(h));
    Gdiplus::BitmapData data{};
    const auto status = bitmap.LockBits(
        &rect,
        Gdiplus::ImageLockModeRead,
        PixelFormat32bppARGB,
        &data
    );
    if (status != Gdiplus::Ok) {
        if (error != nullptr) {
            *error = "이미지 픽셀 잠금 실패: " + path.string();
        }
        return false;
    }

    out_image.width = static_cast<int>(w);
    out_image.height = static_cast<int>(h);
    out_image.channels = 4;
    out_image.pixels.assign(static_cast<size_t>(w) * h * 4u, 0);
    const auto* src_base = static_cast<const uint8_t*>(data.Scan0);
    const int stride = data.Stride;
    for (UINT y = 0; y < h; ++y) {
        const auto* src = src_base + static_cast<ptrdiff_t>(y) * stride;
        auto* dst = out_image.pixels.data() + static_cast<size_t>(y) * w * 4u;
        std::memcpy(dst, src, static_cast<size_t>(w) * 4u);
    }
    bitmap.UnlockBits(&data);
    if (debug_enabled()) {
        debug_log(std::string("load_gdiplus path=") + path.string() +
                  ", width=" + std::to_string(w) +
                  ", height=" + std::to_string(h) +
                  ", bgra0=" + std::to_string(out_image.pixels[0]) + "," +
                  std::to_string(out_image.pixels[1]) + "," +
                  std::to_string(out_image.pixels[2]) + "," +
                  std::to_string(out_image.pixels[3]));
    }
    return true;
}
#endif

bool load_image_file(const fs::path& path, Image& out_image, std::string* error) {
#if defined(BUZHIDAO_HAVE_OPENCV)
    cv::Mat image = cv::imread(path.string(), cv::IMREAD_COLOR);
    if (!image.empty()) {
        out_image = cv_mat_to_image_bgra(image);
        if (out_image.width > 0 && out_image.height > 0) {
            return true;
        }
    }
#endif
    const auto ext = to_lower_ascii(path.extension().string());
    if (ext == ".bmp") {
        return load_bmp(path, out_image, error);
    }
#if defined(_WIN32)
    return load_bitmap_with_gdiplus(path, out_image, error);
#else
    if (error != nullptr) {
        *error = "BMP 외 이미지 포맷은 현재 지원하지 않습니다: " + path.string();
    }
    return false;
#endif
}

Image resize_bilinear(const Image& img, int target_w, int target_h) {
#if defined(BUZHIDAO_HAVE_OPENCV)
    if (target_w <= 0 || target_h <= 0 || img.width <= 0 || img.height <= 0) {
        return {0, 0, 0, {}};
    }
    if (img.width == target_w && img.height == target_h) {
        return img;
    }
    cv::Mat resized;
    cv::resize(
        image_to_cv_mat_bgr(img),
        resized,
        cv::Size(target_w, target_h),
        0.0,
        0.0,
        cv::INTER_LINEAR
    );
    return cv_mat_to_image_bgra(resized);
#else
    if (target_w <= 0 || target_h <= 0 || img.width <= 0 || img.height <= 0) {
        return {0, 0, 0, {}};
    }
    if (img.width == target_w && img.height == target_h) {
        return img;
    }
    Image out{target_w, target_h, img.channels, std::vector<uint8_t>(target_w * target_h * 4)};
    const float scale_x = static_cast<float>(img.width) / static_cast<float>(target_w);
    const float scale_y = static_cast<float>(img.height) / static_cast<float>(target_h);

    for (int y = 0; y < target_h; ++y) {
        const float gy = (static_cast<float>(y) + 0.5f) * scale_y - 0.5f;
        for (int x = 0; x < target_w; ++x) {
            const float gx = (static_cast<float>(x) + 0.5f) * scale_x - 0.5f;
            const size_t t = static_cast<size_t>((y * target_w + x) * 4);
            for (int c = 0; c < 4; ++c) {
                out.pixels[t + c] = sample_channel_bilinear_replicate(img, gx, gy, c);
            }
        }
    }

    return out;
#endif
}

Image load_oriented_image(const fs::path& path, std::string* err) {
    Image img{0, 0, 0, {}};
    if (!load_bmp(path, img, err)) {
        return img;
    }
    return img;
}

Image resize_for_det(
    const Image& img,
    int det_limit_side_len,
    const std::string& det_limit_type,
    int det_max_side_limit,
    int* resized_h,
    int* resized_w
) {
    if (img.width <= 0 || img.height <= 0) {
        return img;
    }
    Image working = img;
    if (img.width + img.height < 64) {
        const int padded_w = std::max(32, img.width);
        const int padded_h = std::max(32, img.height);
        working = Image{
            padded_w,
            padded_h,
            img.channels,
            std::vector<uint8_t>(static_cast<size_t>(padded_w * padded_h) * 4, 0)
        };
        for (int y = 0; y < img.height; ++y) {
            for (int x = 0; x < img.width; ++x) {
                const size_t src = static_cast<size_t>(y * img.width + x) * 4;
                const size_t dst = static_cast<size_t>(y * padded_w + x) * 4;
                working.pixels[dst + 0] = img.pixels[src + 0];
                working.pixels[dst + 1] = img.pixels[src + 1];
                working.pixels[dst + 2] = img.pixels[src + 2];
                working.pixels[dst + 3] = img.pixels[src + 3];
            }
        }
    }
    const int src_w = working.width;
    const int src_h = working.height;
    const int max_side = std::max(src_w, src_h);
    const int min_side = std::min(src_w, src_h);
    const int target_side = std::max(det_limit_side_len, 1);
    const int max_side_limit = std::max(det_max_side_limit, target_side);
    const std::string limit_type = to_lower_ascii(det_limit_type);

    float scale = 1.0f;
    if (limit_type == "max") {
        if (max_side > target_side) {
            scale = static_cast<float>(target_side) / static_cast<float>(max_side);
        }
    } else if (limit_type == "min") {
        if (min_side < target_side) {
            scale = static_cast<float>(target_side) / static_cast<float>(min_side);
        }
    } else if (limit_type == "resize_long") {
        scale = static_cast<float>(target_side) / static_cast<float>(max_side);
    } else {
        if (min_side < target_side) {
            scale = static_cast<float>(target_side) / static_cast<float>(min_side);
        }
    }

    int new_w = static_cast<int>(src_w * scale);
    int new_h = static_cast<int>(src_h * scale);
    if (std::max(new_h, new_w) > max_side_limit) {
        const float clamp_scale = static_cast<float>(max_side_limit) /
                                  static_cast<float>(std::max(new_h, new_w));
        new_h = static_cast<int>(new_h * clamp_scale);
        new_w = static_cast<int>(new_w * clamp_scale);
    }
    new_w = std::max(static_cast<int>(DET_ALIGN), static_cast<int>(std::round(static_cast<float>(new_w) / DET_ALIGN) * DET_ALIGN));
    new_h = std::max(static_cast<int>(DET_ALIGN), static_cast<int>(std::round(static_cast<float>(new_h) / DET_ALIGN) * DET_ALIGN));
    if (resized_w != nullptr) {
        *resized_w = new_w;
    }
    if (resized_h != nullptr) {
        *resized_h = new_h;
    }
    if (new_w == src_w && new_h == src_h) {
        return working;
    }
    return resize_bilinear(working, new_w, new_h);
}

float point_distance(const FloatPoint& a, const FloatPoint& b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
}

FloatPoint lerp_point(const FloatPoint& a, const FloatPoint& b, float t) {
    return {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
    };
}

float cross(const FloatPoint& o, const FloatPoint& a, const FloatPoint& b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

std::vector<FloatPoint> convex_hull(std::vector<FloatPoint> points) {
    if (points.size() <= 1) {
        return points;
    }
    std::sort(points.begin(), points.end(), [](const FloatPoint& a, const FloatPoint& b) {
        if (a.x != b.x) {
            return a.x < b.x;
        }
        return a.y < b.y;
    });
    points.erase(std::unique(points.begin(), points.end(), [](const FloatPoint& a, const FloatPoint& b) {
        return std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f;
    }), points.end());
    if (points.size() <= 2) {
        return points;
    }

    std::vector<FloatPoint> lower;
    for (const auto& p : points) {
        while (lower.size() >= 2 &&
               cross(lower[lower.size() - 2], lower.back(), p) <= 0.0f) {
            lower.pop_back();
        }
        lower.push_back(p);
    }

    std::vector<FloatPoint> upper;
    for (auto it = points.rbegin(); it != points.rend(); ++it) {
        while (upper.size() >= 2 &&
               cross(upper[upper.size() - 2], upper.back(), *it) <= 0.0f) {
            upper.pop_back();
        }
        upper.push_back(*it);
    }

    lower.pop_back();
    upper.pop_back();
    lower.insert(lower.end(), upper.begin(), upper.end());
    return lower;
}

std::array<FloatPoint, 4> rect_to_points(const OrientedRect& rect) {
    const float cos_a = std::cos(rect.angle);
    const float sin_a = std::sin(rect.angle);
    const float dx = rect.width * 0.5f;
    const float dy = rect.height * 0.5f;
    const FloatPoint ux{cos_a, sin_a};
    const FloatPoint uy{-sin_a, cos_a};
    return {{
        {rect.center.x - ux.x * dx - uy.x * dy, rect.center.y - ux.y * dx - uy.y * dy},
        {rect.center.x + ux.x * dx - uy.x * dy, rect.center.y + ux.y * dx - uy.y * dy},
        {rect.center.x + ux.x * dx + uy.x * dy, rect.center.y + ux.y * dx + uy.y * dy},
        {rect.center.x - ux.x * dx + uy.x * dy, rect.center.y - ux.y * dx + uy.y * dy},
    }};
}

std::array<FloatPoint, 4> order_clockwise(const std::array<FloatPoint, 4>& pts) {
    std::array<FloatPoint, 4> ordered = pts;
    FloatPoint center{0.0f, 0.0f};
    for (const auto& p : pts) {
        center.x += p.x;
        center.y += p.y;
    }
    center.x /= 4.0f;
    center.y /= 4.0f;
    std::sort(ordered.begin(), ordered.end(), [&](const FloatPoint& a, const FloatPoint& b) {
        const float angle_a = std::atan2(a.y - center.y, a.x - center.x);
        const float angle_b = std::atan2(b.y - center.y, b.x - center.x);
        return angle_a < angle_b;
    });
    size_t tl = 0;
    float best = ordered[0].x + ordered[0].y;
    for (size_t i = 1; i < ordered.size(); ++i) {
        const float score = ordered[i].x + ordered[i].y;
        if (score < best) {
            best = score;
            tl = i;
        }
    }
    std::array<FloatPoint, 4> rotated{};
    for (size_t i = 0; i < 4; ++i) {
        rotated[i] = ordered[(tl + i) % 4];
    }
    if (cross(rotated[0], rotated[1], rotated[2]) < 0.0f) {
        std::swap(rotated[1], rotated[3]);
    }
    return rotated;
}

std::array<FloatPoint, 4> order_crop_box_like_sidecar(const std::array<FloatPoint, 4>& pts) {
    std::array<FloatPoint, 4> sorted = pts;
    std::sort(sorted.begin(), sorted.end(), [](const FloatPoint& a, const FloatPoint& b) {
        if (std::fabs(a.x - b.x) > 1e-4f) {
            return a.x < b.x;
        }
        return a.y < b.y;
    });

    size_t index_a = 0;
    size_t index_b = 2;
    size_t index_c = 3;
    size_t index_d = 1;
    if (sorted[1].y > sorted[0].y) {
        index_a = 0;
        index_d = 1;
    } else {
        index_a = 1;
        index_d = 0;
    }
    if (sorted[3].y > sorted[2].y) {
        index_b = 2;
        index_c = 3;
    } else {
        index_b = 3;
        index_c = 2;
    }
    return {{
        sorted[index_a],
        sorted[index_b],
        sorted[index_c],
        sorted[index_d],
    }};
}

std::pair<std::array<FloatPoint, 4>, float> get_mini_box(const std::array<FloatPoint, 4>& pts) {
    std::array<FloatPoint, 4> sorted = pts;
    std::sort(sorted.begin(), sorted.end(), [](const FloatPoint& a, const FloatPoint& b) {
        if (std::fabs(a.x - b.x) > 1e-4f) {
            return a.x < b.x;
        }
        return a.y < b.y;
    });

    std::array<FloatPoint, 2> left = {sorted[0], sorted[1]};
    std::array<FloatPoint, 2> right = {sorted[2], sorted[3]};
    if (left[0].y > left[1].y) {
        std::swap(left[0], left[1]);
    }
    if (right[0].y > right[1].y) {
        std::swap(right[0], right[1]);
    }

    std::array<FloatPoint, 4> box = {{
        left[0],
        right[0],
        right[1],
        left[1],
    }};
    const float side1 = point_distance(box[0], box[1]);
    const float side2 = point_distance(box[0], box[3]);
    return {box, std::min(side1, side2)};
}

std::pair<std::array<FloatPoint, 4>, float> get_mini_box(const OrientedRect& rect) {
    return get_mini_box(rect_to_points(rect));
}

OrientedRect min_area_rect(const std::vector<FloatPoint>& hull) {
    if (hull.empty()) {
        return {{0.0f, 0.0f}, 0.0f, 0.0f, 0.0f};
    }
    if (hull.size() == 1) {
        return {hull[0], 1.0f, 1.0f, 0.0f};
    }

    float best_area = std::numeric_limits<float>::max();
    OrientedRect best{};
    for (size_t i = 0; i < hull.size(); ++i) {
        const auto& p0 = hull[i];
        const auto& p1 = hull[(i + 1) % hull.size()];
        const float angle = std::atan2(p1.y - p0.y, p1.x - p0.x);
        const float cos_a = std::cos(angle);
        const float sin_a = std::sin(angle);
        float min_x = std::numeric_limits<float>::max();
        float max_x = std::numeric_limits<float>::lowest();
        float min_y = std::numeric_limits<float>::max();
        float max_y = std::numeric_limits<float>::lowest();
        for (const auto& p : hull) {
            const float rx = p.x * cos_a + p.y * sin_a;
            const float ry = -p.x * sin_a + p.y * cos_a;
            min_x = std::min(min_x, rx);
            max_x = std::max(max_x, rx);
            min_y = std::min(min_y, ry);
            max_y = std::max(max_y, ry);
        }
        const float width = std::max(1e-3f, max_x - min_x);
        const float height = std::max(1e-3f, max_y - min_y);
        const float area = width * height;
        if (area >= best_area) {
            continue;
        }
        const float cx = (min_x + max_x) * 0.5f;
        const float cy = (min_y + max_y) * 0.5f;
        best_area = area;
        best.center = {
            cx * cos_a - cy * sin_a,
            cx * sin_a + cy * cos_a,
        };
        best.width = width;
        best.height = height;
        best.angle = angle;
    }
    return best;
}

MinAreaRectBox min_area_rect_box_like_opencv(const std::vector<FloatPoint>& points) {
    MinAreaRectBox result{{}, {{0.0f, 0.0f}, 0.0f, 0.0f, -90.0f}};
    if (points.empty()) {
        return result;
    }
#if defined(BUZHIDAO_HAVE_OPENCV)
    std::vector<cv::Point2f> contour;
    contour.reserve(points.size());
    for (const auto& point : points) {
        contour.emplace_back(point.x, point.y);
    }
    const cv::RotatedRect rr = cv::minAreaRect(contour);
    cv::Point2f cv_points[4];
    rr.points(cv_points);
    result.rect.center = {rr.center.x, rr.center.y};
    result.rect.width = rr.size.width;
    result.rect.height = rr.size.height;
    result.rect.angle = rr.angle;
    for (int i = 0; i < 4; ++i) {
        result.corners[static_cast<size_t>(i)] = {cv_points[i].x, cv_points[i].y};
    }
    return result;
#endif

    std::vector<FloatPoint> hull = convex_hull(points);
    if (hull.empty()) {
        hull = points;
    }
    const int n = static_cast<int>(hull.size());
    if (n == 1) {
        result.rect.center = hull[0];
        result.corners = {{hull[0], hull[0], hull[0], hull[0]}};
        return result;
    }
    if (n == 2) {
        const FloatPoint center{
            (hull[0].x + hull[1].x) * 0.5f,
            (hull[0].y + hull[1].y) * 0.5f,
        };
        const float dx = hull[0].x - hull[1].x;
        const float dy = hull[0].y - hull[1].y;
        result.rect.center = center;
        result.rect.width = 0.0f;
        result.rect.height = std::sqrt(dx * dx + dy * dy);
        if (dx == 0.0f) {
            std::swap(result.rect.width, result.rect.height);
        } else if (dy < 0.0f) {
            result.rect.angle = static_cast<float>(std::atan2(dy, dx) * 180.0 / std::acos(-1.0));
            std::swap(result.rect.width, result.rect.height);
        } else if (dy > 0.0f) {
            result.rect.angle = static_cast<float>(-std::atan2(dx, dy) * 180.0 / std::acos(-1.0));
        }
        result.corners = rect_to_points(result.rect);
        return result;
    }

    std::vector<FloatPoint> vect(static_cast<size_t>(n));
    std::vector<float> inv_vect_length(static_cast<size_t>(n), 0.0f);
    int left = 0;
    int bottom = 0;
    int right = 0;
    int top = 0;
    float left_x = hull[0].x;
    float right_x = hull[0].x;
    float top_y = hull[0].y;
    float bottom_y = hull[0].y;
    FloatPoint pt0 = hull[0];
    for (int i = 0; i < n; ++i) {
        if (pt0.x < left_x) {
            left_x = pt0.x;
            left = i;
        }
        if (pt0.x > right_x) {
            right_x = pt0.x;
            right = i;
        }
        if (pt0.y > top_y) {
            top_y = pt0.y;
            top = i;
        }
        if (pt0.y < bottom_y) {
            bottom_y = pt0.y;
            bottom = i;
        }
        const FloatPoint pt = hull[(i + 1) < n ? (i + 1) : 0];
        const double dx = static_cast<double>(pt.x) - static_cast<double>(pt0.x);
        const double dy = static_cast<double>(pt.y) - static_cast<double>(pt0.y);
        vect[static_cast<size_t>(i)] = {static_cast<float>(dx), static_cast<float>(dy)};
        inv_vect_length[static_cast<size_t>(i)] =
            static_cast<float>(1.0 / std::sqrt(dx * dx + dy * dy));
        pt0 = pt;
    }

    float orientation = 0.0f;
    double ax = vect[static_cast<size_t>(n - 1)].x;
    double ay = vect[static_cast<size_t>(n - 1)].y;
    for (int i = 0; i < n; ++i) {
        const double bx = vect[static_cast<size_t>(i)].x;
        const double by = vect[static_cast<size_t>(i)].y;
        const double convexity = ax * by - ay * bx;
        if (convexity != 0.0) {
            orientation = convexity > 0.0 ? 1.0f : -1.0f;
            break;
        }
        ax = bx;
        ay = by;
    }
    if (orientation == 0.0f) {
        orientation = 1.0f;
    }

    auto rotate90ccw = [](const FloatPoint& in) {
        return FloatPoint{-in.y, in.x};
    };
    auto rotate90cw = [](const FloatPoint& in) {
        return FloatPoint{in.y, -in.x};
    };
    auto rotate180p = [](const FloatPoint& in) {
        return FloatPoint{-in.x, -in.y};
    };
    auto first_vec_is_right = [&](const FloatPoint& vec1, const FloatPoint& vec2) {
        const FloatPoint tmp = rotate90cw(vec1);
        return tmp.x * vec2.x + tmp.y * vec2.y < 0.0f;
    };

    float minarea = FLT_MAX;
    std::array<float, 7> buf{};
    std::array<int, 2> buf_i{};
    int seq[4] = {bottom, right, top, left};
    FloatPoint rot_vect[4]{};
    float base_a = orientation;
    float base_b = 0.0f;

    for (int k = 0; k < n; ++k) {
        int main_element = 0;
        rot_vect[0] = vect[static_cast<size_t>(seq[0])];
        rot_vect[1] = rotate90cw(vect[static_cast<size_t>(seq[1])]);
        rot_vect[2] = rotate180p(vect[static_cast<size_t>(seq[2])]);
        rot_vect[3] = rotate90ccw(vect[static_cast<size_t>(seq[3])]);
        for (int i = 1; i < 4; ++i) {
            if (first_vec_is_right(rot_vect[i], rot_vect[main_element])) {
                main_element = i;
            }
        }

        const int pindex = seq[main_element];
        const float lead_x = vect[static_cast<size_t>(pindex)].x * inv_vect_length[static_cast<size_t>(pindex)];
        const float lead_y = vect[static_cast<size_t>(pindex)].y * inv_vect_length[static_cast<size_t>(pindex)];
        switch (main_element) {
        case 0:
            base_a = lead_x;
            base_b = lead_y;
            break;
        case 1:
            base_a = lead_y;
            base_b = -lead_x;
            break;
        case 2:
            base_a = -lead_x;
            base_b = -lead_y;
            break;
        case 3:
            base_a = -lead_y;
            base_b = lead_x;
            break;
        default:
            break;
        }

        seq[main_element] += 1;
        if (seq[main_element] == n) {
            seq[main_element] = 0;
        }

        float dx = hull[static_cast<size_t>(seq[1])].x - hull[static_cast<size_t>(seq[3])].x;
        float dy = hull[static_cast<size_t>(seq[1])].y - hull[static_cast<size_t>(seq[3])].y;
        const float width = dx * base_a + dy * base_b;
        dx = hull[static_cast<size_t>(seq[2])].x - hull[static_cast<size_t>(seq[0])].x;
        dy = hull[static_cast<size_t>(seq[2])].y - hull[static_cast<size_t>(seq[0])].y;
        const float height = -dx * base_b + dy * base_a;
        const float area = width * height;
        if (area <= minarea) {
            minarea = area;
            buf_i[0] = seq[3];
            buf[1] = base_a;
            buf[2] = width;
            buf[3] = base_b;
            buf[4] = height;
            buf_i[1] = seq[0];
            buf[6] = area;
        }
    }

    const float a1 = buf[1];
    const float b1 = buf[3];
    const float a2 = -buf[3];
    const float b2 = buf[1];
    const float c1 = a1 * hull[static_cast<size_t>(buf_i[0])].x + hull[static_cast<size_t>(buf_i[0])].y * b1;
    const float c2 = a2 * hull[static_cast<size_t>(buf_i[1])].x + hull[static_cast<size_t>(buf_i[1])].y * b2;
    const float idet = 1.0f / (a1 * b2 - a2 * b1);
    const FloatPoint corner{
        (c1 * b2 - c2 * b1) * idet,
        (a1 * c2 - a2 * c1) * idet,
    };
    const FloatPoint vec1{a1 * buf[2], b1 * buf[2]};
    const FloatPoint vec2{a2 * buf[4], b2 * buf[4]};
    result.corners = {{
        corner,
        {corner.x + vec1.x, corner.y + vec1.y},
        {corner.x + vec1.x + vec2.x, corner.y + vec1.y + vec2.y},
        {corner.x + vec2.x, corner.y + vec2.y},
    }};
    result.rect.center = {
        corner.x + (vec1.x + vec2.x) * 0.5f,
        corner.y + (vec1.y + vec2.y) * 0.5f,
    };
    result.rect.width = std::sqrt(vec2.x * vec2.x + vec2.y * vec2.y);
    result.rect.height = std::sqrt(vec1.x * vec1.x + vec1.y * vec1.y);
    if (vec1.x == 0.0f && vec1.y > 0.0f) {
        std::swap(result.rect.width, result.rect.height);
    } else {
        result.rect.angle = static_cast<float>(-std::atan2(static_cast<double>(vec1.x), static_cast<double>(vec1.y)) * 180.0 / std::acos(-1.0));
    }
    return result;
}

OrientedRect expand_rect(const OrientedRect& rect, float ratio) {
    if (rect.width <= 0.0f || rect.height <= 0.0f) {
        return rect;
    }
    const float area = rect.width * rect.height;
    const float perimeter = 2.0f * (rect.width + rect.height);
    if (perimeter <= 1e-6f) {
        return rect;
    }
    const float distance = area * ratio / perimeter;
    return {
        rect.center,
        rect.width + distance * 2.0f,
        rect.height + distance * 2.0f,
        rect.angle,
    };
}

bool point_in_quad(const FloatPoint& p, const std::array<FloatPoint, 4>& quad) {
    float prev = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const float c = cross(quad[i], quad[(i + 1) % 4], p);
        if (std::fabs(c) < 1e-5f) {
            continue;
        }
        if (prev == 0.0f) {
            prev = c;
            continue;
        }
        if ((prev > 0.0f) != (c > 0.0f)) {
            return false;
        }
    }
    return true;
}

Image rotate90_counterclockwise(const Image& img) {
    Image out{img.height, img.width, img.channels, std::vector<uint8_t>(static_cast<size_t>(img.width * img.height) * 4)};
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            const int nx = y;
            const int ny = img.width - 1 - x;
            const size_t src = static_cast<size_t>(y * img.width + x) * 4;
            const size_t dst = static_cast<size_t>(ny * out.width + nx) * 4;
            out.pixels[dst + 0] = img.pixels[src + 0];
            out.pixels[dst + 1] = img.pixels[src + 1];
            out.pixels[dst + 2] = img.pixels[src + 2];
            out.pixels[dst + 3] = img.pixels[src + 3];
        }
    }
    return out;
}

float cubic_weight(float x) {
    constexpr float a = -0.75f;
    x = std::fabs(x);
    if (x <= 1.0f) {
        return ((a + 2.0f) * x - (a + 3.0f)) * x * x + 1.0f;
    }
    if (x < 2.0f) {
        return (((a * x - 5.0f * a) * x + 8.0f * a) * x) - 4.0f * a;
    }
    return 0.0f;
}

uint8_t sample_channel_cubic_replicate(const Image& img, float sx, float sy, int channel) {
    const int base_x = static_cast<int>(std::floor(sx));
    const int base_y = static_cast<int>(std::floor(sy));
    float accum = 0.0f;
    float weight_sum = 0.0f;
    for (int dy = -1; dy <= 2; ++dy) {
        const int py = std::clamp(base_y + dy, 0, img.height - 1);
        const float wy = cubic_weight(sy - static_cast<float>(base_y + dy));
        for (int dx = -1; dx <= 2; ++dx) {
            const int px = std::clamp(base_x + dx, 0, img.width - 1);
            const float wx = cubic_weight(sx - static_cast<float>(base_x + dx));
            const float w = wx * wy;
            const size_t idx = (static_cast<size_t>(py) * img.width + px) * 4 + channel;
            accum += w * static_cast<float>(img.pixels[idx]);
            weight_sum += w;
        }
    }
    const float value = weight_sum > 0.0f ? (accum / weight_sum) : 0.0f;
    return static_cast<uint8_t>(std::clamp(static_cast<float>(std::round(value)), 0.0f, 255.0f));
}

uint8_t sample_channel_cubic_constant_zero(const Image& img, float sx, float sy, int channel) {
    const int base_x = static_cast<int>(std::floor(sx));
    const int base_y = static_cast<int>(std::floor(sy));
    float accum = 0.0f;
    float weight_sum = 0.0f;
    for (int dy = -1; dy <= 2; ++dy) {
        const int py = base_y + dy;
        const float wy = cubic_weight(sy - static_cast<float>(base_y + dy));
        for (int dx = -1; dx <= 2; ++dx) {
            const int px = base_x + dx;
            const float wx = cubic_weight(sx - static_cast<float>(base_x + dx));
            const float w = wx * wy;
            if (px >= 0 && px < img.width && py >= 0 && py < img.height) {
                const size_t idx = (static_cast<size_t>(py) * img.width + px) * 4 + channel;
                accum += w * static_cast<float>(img.pixels[idx]);
            }
            weight_sum += w;
        }
    }
    const float value = weight_sum > 0.0f ? (accum / weight_sum) : 0.0f;
    return static_cast<uint8_t>(std::clamp(static_cast<float>(std::round(value)), 0.0f, 255.0f));
}

uint8_t sample_channel_bilinear_replicate(const Image& img, float sx, float sy, int channel) {
    const float gx = std::clamp(sx, 0.0f, static_cast<float>(img.width - 1));
    const float gy = std::clamp(sy, 0.0f, static_cast<float>(img.height - 1));
    const int x0 = std::clamp(static_cast<int>(std::floor(gx)), 0, img.width - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(gy)), 0, img.height - 1);
    const int x1 = std::clamp(x0 + 1, 0, img.width - 1);
    const int y1 = std::clamp(y0 + 1, 0, img.height - 1);
    const float wx = std::clamp(gx - std::floor(gx), 0.0f, 1.0f);
    const float wy = std::clamp(gy - std::floor(gy), 0.0f, 1.0f);
    const float wx1 = 1.0f - wx;
    const float wy1 = 1.0f - wy;
    const size_t d0 = static_cast<size_t>((y0 * img.width + x0) * 4 + channel);
    const size_t d1 = static_cast<size_t>((y0 * img.width + x1) * 4 + channel);
    const size_t d2 = static_cast<size_t>((y1 * img.width + x0) * 4 + channel);
    const size_t d3 = static_cast<size_t>((y1 * img.width + x1) * 4 + channel);
    const float p0 = img.pixels[d0] * wx1 + img.pixels[d1] * wx;
    const float p1 = img.pixels[d2] * wx1 + img.pixels[d3] * wx;
    const float p = p0 * wy1 + p1 * wy;
    return static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::round(p)), 0, 255));
}

#if defined(BUZHIDAO_HAVE_OPENCV)
Image crop_to_bbox(const cv::Mat& img_bgr, const std::array<FloatPoint, 4>& pts, std::string* err) {
    if (img_bgr.empty()) {
        if (err != nullptr) {
            *err = "crop source image가 비어 있습니다";
        }
        return {0, 0, 0, {}};
    }
    std::vector<FloatPoint> crop_points(pts.begin(), pts.end());
    for (auto& point : crop_points) {
        point.x = static_cast<float>(static_cast<int>(point.x));
        point.y = static_cast<float>(static_cast<int>(point.y));
    }
    const auto crop_box = min_area_rect_box_like_opencv(crop_points);
    const auto quad = order_crop_box_like_sidecar(crop_box.corners);
    const float top_w = point_distance(quad[0], quad[1]);
    const float bottom_w = point_distance(quad[3], quad[2]);
    const float left_h = point_distance(quad[0], quad[3]);
    const float right_h = point_distance(quad[1], quad[2]);
    const int out_w = std::max(1, static_cast<int>(std::max(top_w, bottom_w)));
    const int out_h = std::max(1, static_cast<int>(std::max(left_h, right_h)));
    if (out_w <= 0 || out_h <= 0) {
        if (err != nullptr) {
            *err = "detected box가 너무 작습니다";
        }
        return {0, 0, 0, {}};
    }
    cv::Point2f src_points[4] = {
        cv::Point2f(quad[0].x, quad[0].y),
        cv::Point2f(quad[1].x, quad[1].y),
        cv::Point2f(quad[2].x, quad[2].y),
        cv::Point2f(quad[3].x, quad[3].y),
    };
    cv::Point2f dst_points[4] = {
        cv::Point2f(0.0f, 0.0f),
        cv::Point2f(static_cast<float>(out_w), 0.0f),
        cv::Point2f(static_cast<float>(out_w), static_cast<float>(out_h)),
        cv::Point2f(0.0f, static_cast<float>(out_h)),
    };
    cv::Mat transform = cv::getPerspectiveTransform(src_points, dst_points);
    cv::Mat cropped;
    cv::warpPerspective(
        img_bgr,
        cropped,
        transform,
        cv::Size(out_w, out_h),
        cv::INTER_CUBIC,
        cv::BORDER_REPLICATE
    );
    const bool crop_dump_enabled = !debug_dump_dir().empty();
    if (crop_dump_enabled) {
        Image warped = cv_mat_to_image_bgra(cropped);
        dump_crop_stage_if_enabled("crop_warp", pts, quad, out_w, out_h, warped);
    }
    if (static_cast<float>(cropped.rows) / static_cast<float>(cropped.cols) >= 1.5f) {
        cv::rotate(cropped, cropped, cv::ROTATE_90_COUNTERCLOCKWISE);
    }
    Image out = cv_mat_to_image_bgra(cropped);
    if (crop_dump_enabled) {
        dump_crop_stage_if_enabled("crop_final", pts, quad, out.width, out.height, out);
    }
    return out;
}
#endif

Image crop_to_bbox(const Image& img, const std::array<FloatPoint, 4>& pts, std::string* err) {
#if defined(BUZHIDAO_HAVE_OPENCV)
    cv::Mat src_bgr = image_to_cv_mat_bgr(img);
    if (src_bgr.empty()) {
        if (err != nullptr) {
            *err = "OpenCV BGR 이미지 변환에 실패했습니다";
        }
        return {0, 0, 0, {}};
    }
    return crop_to_bbox(src_bgr, pts, err);
#else
    std::vector<FloatPoint> crop_points(pts.begin(), pts.end());
    for (auto& point : crop_points) {
        point.x = static_cast<float>(static_cast<int>(point.x));
        point.y = static_cast<float>(static_cast<int>(point.y));
    }
    const auto crop_box = min_area_rect_box_like_opencv(crop_points);
    const auto quad = order_crop_box_like_sidecar(crop_box.corners);
    const float top_w = point_distance(quad[0], quad[1]);
    const float bottom_w = point_distance(quad[3], quad[2]);
    const float left_h = point_distance(quad[0], quad[3]);
    const float right_h = point_distance(quad[1], quad[2]);
    const int out_w = std::max(1, static_cast<int>(std::max(top_w, bottom_w)));
    const int out_h = std::max(1, static_cast<int>(std::max(left_h, right_h)));
    if (out_w <= 0 || out_h <= 0) {
        if (err != nullptr) {
            *err = "detected box가 너무 작습니다";
        }
        return {0, 0, 0, {}};
    }
    cv::Point2f src_points[4] = {
        cv::Point2f(quad[0].x, quad[0].y),
        cv::Point2f(quad[1].x, quad[1].y),
        cv::Point2f(quad[2].x, quad[2].y),
        cv::Point2f(quad[3].x, quad[3].y),
    };
    cv::Point2f dst_points[4] = {
        cv::Point2f(0.0f, 0.0f),
        cv::Point2f(static_cast<float>(out_w), 0.0f),
        cv::Point2f(static_cast<float>(out_w), static_cast<float>(out_h)),
        cv::Point2f(0.0f, static_cast<float>(out_h)),
    };
    cv::Mat transform = cv::getPerspectiveTransform(src_points, dst_points);
    cv::Mat cropped;
    cv::warpPerspective(
        image_to_cv_mat_bgr(img),
        cropped,
        transform,
        cv::Size(out_w, out_h),
        cv::INTER_CUBIC,
        cv::BORDER_REPLICATE
    );
    Image warped = cv_mat_to_image_bgra(cropped);
    dump_crop_stage_if_enabled("crop_warp", pts, quad, out_w, out_h, warped);
    if (static_cast<float>(cropped.rows) / static_cast<float>(cropped.cols) >= 1.5f) {
        cv::rotate(cropped, cropped, cv::ROTATE_90_COUNTERCLOCKWISE);
    }
    int out_w = std::max(1, static_cast<int>(std::max(top_w, bottom_w)));
    int out_h = std::max(1, static_cast<int>(std::max(left_h, right_h)));
    if (out_w <= 0 || out_h <= 0) {
        if (err != nullptr) {
            *err = "detected box가 너무 작습니다";
        }
        return {0, 0, 0, {}};
    }
    Image out{out_w, out_h, img.channels, std::vector<uint8_t>(static_cast<size_t>(out_w * out_h) * 4, 0)};
    const std::array<FloatPoint, 4> dst = {{
        {0.0f, 0.0f},
        {static_cast<float>(out_w), 0.0f},
        {static_cast<float>(out_w), static_cast<float>(out_h)},
        {0.0f, static_cast<float>(out_h)},
    }};
    double a[8][9] = {};
    for (int i = 0; i < 4; ++i) {
        const double x = static_cast<double>(dst[i].x);
        const double y = static_cast<double>(dst[i].y);
        const double u = static_cast<double>(quad[i].x);
        const double v = static_cast<double>(quad[i].y);
        a[i * 2 + 0][0] = x;
        a[i * 2 + 0][1] = y;
        a[i * 2 + 0][2] = 1.0;
        a[i * 2 + 0][6] = -x * u;
        a[i * 2 + 0][7] = -y * u;
        a[i * 2 + 0][8] = u;
        a[i * 2 + 1][3] = x;
        a[i * 2 + 1][4] = y;
        a[i * 2 + 1][5] = 1.0;
        a[i * 2 + 1][6] = -x * v;
        a[i * 2 + 1][7] = -y * v;
        a[i * 2 + 1][8] = v;
    }
    for (int col = 0; col < 8; ++col) {
        int pivot = col;
        for (int row = col + 1; row < 8; ++row) {
            if (std::fabs(a[row][col]) > std::fabs(a[pivot][col])) {
                pivot = row;
            }
        }
        if (std::fabs(a[pivot][col]) < 1e-12) {
            if (err != nullptr) {
                *err = "perspective transform 계산 실패";
            }
            return {0, 0, 0, {}};
        }
        if (pivot != col) {
            for (int j = col; j < 9; ++j) {
                std::swap(a[col][j], a[pivot][j]);
            }
        }
        const double div = a[col][col];
        for (int j = col; j < 9; ++j) {
            a[col][j] /= div;
        }
        for (int row = 0; row < 8; ++row) {
            if (row == col) {
                continue;
            }
            const double factor = a[row][col];
            if (factor == 0.0) {
                continue;
            }
            for (int j = col; j < 9; ++j) {
                a[row][j] -= factor * a[col][j];
            }
        }
    }
    double h[9] = {
        a[0][8], a[1][8], a[2][8],
        a[3][8], a[4][8], a[5][8],
        a[6][8], a[7][8], 1.0,
    };
    for (int y = 0; y < out_h; ++y) {
        for (int x = 0; x < out_w; ++x) {
            const double fx = static_cast<double>(x);
            const double fy = static_cast<double>(y);
            const double denom = h[6] * fx + h[7] * fy + h[8];
            if (std::fabs(denom) < 1e-12) {
                continue;
            }
            const float sx = static_cast<float>((h[0] * fx + h[1] * fy + h[2]) / denom);
            const float sy = static_cast<float>((h[3] * fx + h[4] * fy + h[5]) / denom);
            const size_t dst = static_cast<size_t>(y * out_w + x) * 4;
            out.pixels[dst + 0] = sample_channel_cubic_replicate(img, sx, sy, 0);
            out.pixels[dst + 1] = sample_channel_cubic_replicate(img, sx, sy, 1);
            out.pixels[dst + 2] = sample_channel_cubic_replicate(img, sx, sy, 2);
            out.pixels[dst + 3] = sample_channel_cubic_replicate(img, sx, sy, 3);
        }
    }
    dump_crop_stage_if_enabled("crop_warp", pts, quad, out_w, out_h, out);
    if (static_cast<float>(out.height) / static_cast<float>(out.width) >= 1.5f) {
        out = rotate90_counterclockwise(out);
    }
    dump_crop_stage_if_enabled("crop_final", pts, quad, out.width, out.height, out);
    return out;
#endif
}

std::tuple<std::array<FloatPoint, 4>, int, int> describe_crop_to_bbox(
    const std::array<FloatPoint, 4>& pts
) {
    std::vector<FloatPoint> crop_points(pts.begin(), pts.end());
    for (auto& point : crop_points) {
        point.x = static_cast<float>(static_cast<int>(point.x));
        point.y = static_cast<float>(static_cast<int>(point.y));
    }
    const auto crop_box = min_area_rect_box_like_opencv(crop_points);
    const auto quad = order_crop_box_like_sidecar(crop_box.corners);
    const float top_w = point_distance(quad[0], quad[1]);
    const float bottom_w = point_distance(quad[3], quad[2]);
    const float left_h = point_distance(quad[0], quad[3]);
    const float right_h = point_distance(quad[1], quad[2]);
    const int out_w = std::max(1, static_cast<int>(std::max(top_w, bottom_w)));
    const int out_h = std::max(1, static_cast<int>(std::max(left_h, right_h)));
    return {quad, out_w, out_h};
}

void dump_crop_stage_if_enabled(
    const char* tag,
    const std::array<FloatPoint, 4>& input_pts,
    const std::array<FloatPoint, 4>& quad,
    int out_w,
    int out_h,
    const Image& image
) {
    const std::string dump_dir = debug_dump_dir();
    if (dump_dir.empty()) {
        return;
    }
    std::error_code ec;
    fs::create_directories(fs::path(dump_dir), ec);

    std::ostringstream stem;
    stem << tag
         << "_" << static_cast<int>(std::round(input_pts[0].x))
         << "_" << static_cast<int>(std::round(input_pts[0].y))
         << "_" << out_w << "x" << out_h;

    std::ofstream ofs((fs::path(dump_dir) / (stem.str() + ".json")).string(), std::ios::binary);
    if (ofs) {
        ofs << "{\n";
        ofs << "  \"input_polygon\": [";
        for (size_t i = 0; i < input_pts.size(); ++i) {
            if (i > 0) {
                ofs << ",";
            }
            ofs << "[" << std::setprecision(9) << input_pts[i].x << "," << input_pts[i].y << "]";
        }
        ofs << "],\n";
        ofs << "  \"crop_quad\": [";
        for (size_t i = 0; i < quad.size(); ++i) {
            if (i > 0) {
                ofs << ",";
            }
            ofs << "[" << std::setprecision(9) << quad[i].x << "," << quad[i].y << "]";
        }
        ofs << "],\n";
        ofs << "  \"output_size\": [" << out_w << "," << out_h << "]\n";
        ofs << "}\n";
    }

    std::string dump_err;
    save_bmp(fs::path(dump_dir) / (stem.str() + ".bmp"), image, &dump_err);
}

Image rotate180(const Image& img) {
#if defined(BUZHIDAO_HAVE_OPENCV)
    cv::Mat src = image_to_cv_mat_bgr(img);
    const int h = img.height;
    const int w = img.width;
    const cv::Point2f center(static_cast<float>(w) / 2.0f, static_cast<float>(h) / 2.0f);
    cv::Mat rotation = cv::getRotationMatrix2D(center, 180.0, 1.0);
    const double cos_v = std::abs(rotation.at<double>(0, 0));
    const double sin_v = std::abs(rotation.at<double>(0, 1));
    const int new_w = static_cast<int>((static_cast<double>(h) * sin_v) + (static_cast<double>(w) * cos_v));
    const int new_h = static_cast<int>((static_cast<double>(h) * cos_v) + (static_cast<double>(w) * sin_v));
    rotation.at<double>(0, 2) += (static_cast<double>(new_w) - static_cast<double>(w)) / 2.0;
    rotation.at<double>(1, 2) += (static_cast<double>(new_h) - static_cast<double>(h)) / 2.0;
    cv::Mat rotated;
    cv::warpAffine(
        src,
        rotated,
        rotation,
        cv::Size(new_w, new_h),
        cv::INTER_CUBIC,
        cv::BORDER_CONSTANT
    );
    return cv_mat_to_image_bgra(rotated);
#else
    Image out{img.width, img.height, img.channels, std::vector<uint8_t>(static_cast<size_t>(img.width * img.height) * 4, 0)};
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            const float sx = static_cast<float>(img.width - x);
            const float sy = static_cast<float>(img.height - y);
            const size_t dst = static_cast<size_t>(y * img.width + x) * 4;
            out.pixels[dst + 0] = sample_channel_cubic_constant_zero(img, sx, sy, 0);
            out.pixels[dst + 1] = sample_channel_cubic_constant_zero(img, sx, sy, 1);
            out.pixels[dst + 2] = sample_channel_cubic_constant_zero(img, sx, sy, 2);
            out.pixels[dst + 3] = 255;
        }
    }
    return out;
#endif
}

std::vector<float> preprocess_det(
    const Image& img,
    int det_limit_side_len,
    const std::string& det_limit_type,
    int det_max_side_limit,
    const NormalizeCfg& norm,
    int* out_h,
    int* out_w
) {
    const Image resized = resize_for_det(
        img,
        det_limit_side_len,
        det_limit_type,
        det_max_side_limit,
        out_h,
        out_w
    );
    if (debug_enabled()) {
        debug_log(std::string("preprocess_det src=") + std::to_string(img.width) + "x" +
                  std::to_string(img.height) + ", limit_side_len=" + std::to_string(det_limit_side_len) +
                  ", limit_type=" + det_limit_type +
                  ", max_side_limit=" + std::to_string(det_max_side_limit) +
                  ", resized=" + std::to_string(*out_w) + "x" + std::to_string(*out_h));
    }
    std::vector<float> tensor(1u * 3u * static_cast<size_t>(resized.width * resized.height));
    const float det_scale = norm.scale;
    const float det_std0 = std::fabs(norm.std[0]) > 1e-12f ? norm.std[0] : 1.0f;
    const float det_std1 = std::fabs(norm.std[1]) > 1e-12f ? norm.std[1] : 1.0f;
    const float det_std2 = std::fabs(norm.std[2]) > 1e-12f ? norm.std[2] : 1.0f;
    for (int y = 0; y < resized.height; ++y) {
        for (int x = 0; x < resized.width; ++x) {
            const size_t idx = static_cast<size_t>((y * resized.width + x) * 4);
            const float b = image_blue_at(resized, idx) * det_scale - norm.mean[0];
            const float g = image_green_at(resized, idx) * det_scale - norm.mean[1];
            const float r = image_red_at(resized, idx) * det_scale - norm.mean[2];
            const size_t hw = static_cast<size_t>(y * resized.width + x);
            const size_t hw_stride = static_cast<size_t>(resized.width * resized.height);
            tensor[0 * hw_stride + hw] = b / det_std0;
            tensor[1 * hw_stride + hw] = g / det_std1;
            tensor[2 * hw_stride + hw] = r / det_std2;
        }
    }
    if (debug_enabled() && !tensor.empty()) {
        auto mn = tensor[0];
        auto mx = tensor[0];
        double sum = 0.0;
        double sumsq = 0.0;
        for (const float v : tensor) {
            mn = std::min(mn, v);
            mx = std::max(mx, v);
            sum += v;
            sumsq += v * v;
        }
        const size_t center = tensor.size() / 2;
        debug_log(std::string("preprocess_det tensor stats mn=") + std::to_string(mn) +
                  ", mx=" + std::to_string(mx) + ", mean=" + std::to_string(sum / tensor.size()) +
                  ", rms=" + std::to_string(std::sqrt(sumsq / tensor.size())) +
                  ", center=" + std::to_string(tensor[center]));
    }
    return tensor;
}

void fill_cls_tensor(const Image& resized, const NormalizeCfg& norm, float* dst) {
    const int resized_w = resized.width;
    const int resized_h = resized.height;
    const float cls_scale = norm.scale;
    const float cls_std0 = std::fabs(norm.std[0]) > 1e-12f ? norm.std[0] : 1.0f;
    const float cls_std1 = std::fabs(norm.std[1]) > 1e-12f ? norm.std[1] : 1.0f;
    const float cls_std2 = std::fabs(norm.std[2]) > 1e-12f ? norm.std[2] : 1.0f;
    const size_t hw_stride = static_cast<size_t>(resized_w * resized_h);
    for (int y = 0; y < resized.height; ++y) {
        for (int x = 0; x < resized.width; ++x) {
            const size_t idx = static_cast<size_t>((y * resized.width + x) * 4);
            const float r = image_red_at(resized, idx) * cls_scale - norm.mean[0];
            const float g = image_green_at(resized, idx) * cls_scale - norm.mean[1];
            const float b = image_blue_at(resized, idx) * cls_scale - norm.mean[2];
            const size_t hw = static_cast<size_t>(y * resized.width + x);
            dst[0 * hw_stride + hw] = r / cls_std0;
            dst[1 * hw_stride + hw] = g / cls_std1;
            dst[2 * hw_stride + hw] = b / cls_std2;
        }
    }
}

std::vector<float> preprocess_cls(const Image& img, int target_w, int target_h, const NormalizeCfg& norm) {
    const int resized_w = std::max(1, target_w);
    const int resized_h = std::max(1, target_h);
    const Image resized = resize_bilinear(img, resized_w, resized_h);
    std::vector<float> tensor(1u * 3u * resized_h * resized_w);
    fill_cls_tensor(resized, norm, tensor.data());
    return tensor;
}

Image resize_rec_input_image(
    const Image& img,
    int target_h,
    int target_w,
    int max_w,
    int& out_w
) {
    const int rec_h = std::max(1, target_h);
    const int rec_base_w = std::max(1, target_w);
    const int rec_max_w = std::max(rec_base_w, max_w);
    const float ratio = static_cast<float>(img.width) / std::max(1, img.height);
    const float base_ratio = static_cast<float>(rec_base_w) / rec_h;
    const float max_wh_ratio = std::max(base_ratio, ratio);
    int dynamic_w = static_cast<int>(rec_h * max_wh_ratio);
    dynamic_w = std::max(rec_base_w, dynamic_w);
    dynamic_w = std::min(rec_max_w, dynamic_w);
    int resized_w = 0;
    if (dynamic_w >= rec_max_w) {
        resized_w = rec_max_w;
        dynamic_w = rec_max_w;
    } else if (static_cast<int>(std::ceil(rec_h * ratio)) > dynamic_w) {
        resized_w = dynamic_w;
    } else {
        resized_w = static_cast<int>(std::ceil(rec_h * ratio));
    }
    resized_w = std::max(1, resized_w);
    const Image resized = resize_bilinear(img, resized_w, rec_h);
    out_w = dynamic_w;
    return resized;
}

Image pad_rec_input_image(const Image& resized, int tensor_w) {
    Image canvas{
        tensor_w,
        resized.height,
        resized.channels,
        std::vector<uint8_t>(static_cast<size_t>(tensor_w * resized.height) * 4, 0)
    };
    const size_t row_bytes = static_cast<size_t>(resized.width) * 4u;
    for (int y = 0; y < resized.height; ++y) {
        const auto* src = resized.pixels.data() + static_cast<size_t>(y) * row_bytes;
        auto* dst = canvas.pixels.data() + static_cast<size_t>(y * tensor_w) * 4u;
        std::memcpy(dst, src, row_bytes);
    }
    return canvas;
}

int estimate_rec_input_width(
    int image_w,
    int image_h,
    int target_h,
    int target_w,
    int max_w
) {
    const int rec_h = std::max(1, target_h);
    const int rec_base_w = std::max(1, target_w);
    const int rec_max_w = std::max(rec_base_w, max_w);
    const float ratio = static_cast<float>(std::max(1, image_w)) / std::max(1, image_h);
    const float base_ratio = static_cast<float>(rec_base_w) / rec_h;
    const float max_wh_ratio = std::max(base_ratio, ratio);
    int dynamic_w = static_cast<int>(rec_h * max_wh_ratio);
    dynamic_w = std::max(rec_base_w, dynamic_w);
    dynamic_w = std::min(rec_max_w, dynamic_w);
    return dynamic_w;
}

void fill_rec_tensor(const Image& resized, const NormalizeCfg& norm, int tensor_w, float* dst) {
    const float rec_scale = norm.scale;
    const float rec_std0 = std::fabs(norm.std[0]) > 1e-12f ? norm.std[0] : 1.0f;
    const float rec_std1 = std::fabs(norm.std[1]) > 1e-12f ? norm.std[1] : 1.0f;
    const float rec_std2 = std::fabs(norm.std[2]) > 1e-12f ? norm.std[2] : 1.0f;
    const float pad_b = 0.0f;
    const float pad_g = 0.0f;
    const float pad_r = 0.0f;
    if (debug_enabled()) {
        debug_log("preprocess_rec pad_values=" +
                  std::to_string(pad_b) + "," +
                  std::to_string(pad_g) + "," +
                  std::to_string(pad_r));
    }
    const size_t hw_stride = static_cast<size_t>(tensor_w * resized.height);
    std::fill(dst, dst + hw_stride, pad_b);
    std::fill(dst + hw_stride, dst + hw_stride * 2u, pad_g);
    std::fill(dst + hw_stride * 2u, dst + hw_stride * 3u, pad_r);
    for (int y = 0; y < resized.height; ++y) {
        for (int x = 0; x < resized.width; ++x) {
            const size_t idx = static_cast<size_t>((y * resized.width + x) * 4);
            if (resized.pixels[idx + 3] == 0) {
                continue;
            }
            const size_t hw = static_cast<size_t>(y * tensor_w + x);
            const float b = image_blue_at(resized, idx) * rec_scale - norm.mean[0];
            const float g = image_green_at(resized, idx) * rec_scale - norm.mean[1];
            const float r = image_red_at(resized, idx) * rec_scale - norm.mean[2];
            dst[0 * hw_stride + hw] = b / rec_std0;
            dst[1 * hw_stride + hw] = g / rec_std1;
            dst[2 * hw_stride + hw] = r / rec_std2;
        }
    }
}

std::vector<float> preprocess_rec(
    const Image& img,
    int target_h,
    int target_w,
    int max_w,
    const NormalizeCfg& norm,
    int& out_w
) {
    const Image resized = resize_rec_input_image(img, target_h, target_w, max_w, out_w);
    std::vector<float> tensor(1u * 3u * static_cast<size_t>(resized.height * out_w), 0.0f);
    fill_rec_tensor(resized, norm, out_w, tensor.data());
    return tensor;
}

std::vector<int> infer_shape_as_ints(const std::vector<int64_t>& shape64) {
    std::vector<int> out;
    out.reserve(shape64.size());
    for (const auto dim : shape64) {
        if (dim > 0) {
            out.push_back(static_cast<int>(dim));
        } else {
            out.push_back(0);
        }
    }
    return out;
}

template <typename T>
std::vector<int> shape_to_ints(const std::vector<T>& shape) {
    std::vector<int> out;
    out.reserve(shape.size());
    for (auto d : shape) {
        if (d > 0) {
            out.push_back(static_cast<int>(d));
        } else {
            out.push_back(0);
        }
    }
    return out;
}

size_t shape_elements(const std::vector<int>& shape) {
    size_t n = 1;
    for (const auto dim : shape) {
        if (dim <= 0) {
            return 0;
        }
        n *= static_cast<size_t>(dim);
    }
    return n;
}

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
PredictorIoNames resolve_predictor_io_names(
    const std::shared_ptr<paddle_infer::Predictor>& predictor
) {
    static std::mutex cache_mutex;
    static std::unordered_map<const void*, PredictorIoNames> cache;
    if (!predictor) {
        return {};
    }
    const void* const key = predictor.get();
    {
        const std::lock_guard<std::mutex> lock(cache_mutex);
        const auto it = cache.find(key);
        if (it != cache.end()) {
            return it->second;
        }
    }

    PredictorIoNames names;
    const auto input_names = predictor->GetInputNames();
    if (!input_names.empty()) {
        names.input_name = input_names.front();
    }
    const auto output_names = predictor->GetOutputNames();
    if (!output_names.empty()) {
        names.output_name = output_names.front();
    }
    if (!names.input_name.empty() && !names.output_name.empty()) {
        const std::lock_guard<std::mutex> lock(cache_mutex);
        cache.emplace(key, names);
    }
    return names;
}

bool run_predictor(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const std::vector<float>& input,
    const std::vector<int>& shape,
    std::vector<float>& output,
    std::vector<int>& output_shape,
    std::string* err
) {
    try {
        if (!predictor) {
            if (err != nullptr) {
                *err = "predictor가 없습니다";
            }
            return false;
        }
        const PredictorIoNames io_names = resolve_predictor_io_names(predictor);
        if (io_names.input_name.empty()) {
            if (err != nullptr) {
                *err = "모델 입력 노드가 비어 있습니다";
            }
            return false;
        }
        if (debug_enabled()) {
            std::string shape_str;
            for (size_t i = 0; i < shape.size(); ++i) {
                if (i > 0) {
                    shape_str += "x";
                }
                shape_str += std::to_string(shape[i]);
            }
            debug_log("run_predictor input_name=" + io_names.input_name +
                      ", shape=" + shape_str +
                      ", input_len=" + std::to_string(input.size()));
            if (!input.empty()) {
                debug_log("run_predictor input sample=" +
                          std::to_string(input[0]) + ", " +
                          std::to_string(input[input.size() / 2]) + ", " +
                          std::to_string(input.back()));
            }
        }

        auto input_handle = predictor->GetInputHandle(io_names.input_name);
        if (!input_handle) {
            if (err != nullptr) {
                *err = "입력 핸들을 확보하지 못했습니다";
            }
            return false;
        }
        input_handle->Reshape(shape);
        input_handle->CopyFromCpu(input.data());

        if (!predictor->Run()) {
            if (err != nullptr) {
                *err = "predictor 실행 실패";
            }
            return false;
        }

        if (io_names.output_name.empty()) {
            if (err != nullptr) {
                *err = "모델 출력 노드가 비어 있습니다";
            }
            return false;
        }
        if (debug_enabled()) {
            debug_log("run_predictor output_name=" + io_names.output_name);
        }

        auto output_handle = predictor->GetOutputHandle(io_names.output_name);
        if (!output_handle) {
            if (err != nullptr) {
                *err = "출력 핸들을 확보하지 못했습니다";
            }
            return false;
        }
        auto output_shape64 = output_handle->shape();
        output_shape = shape_to_ints(output_shape64);
        const size_t n = shape_elements(output_shape);
        if (n == 0) {
            output.clear();
            return true;
        }
        output.resize(n);
        output_handle->CopyToCpu(output.data());
        return true;
    } catch (const std::exception& ex) {
        if (err != nullptr) {
            *err = std::string("predictor 실행 예외: ") + ex.what();
        }
        return false;
    }
}

bool run_predictor_into_buffer(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const float* input_data,
    size_t input_len,
    const std::vector<int>& shape,
    FloatScratchBuffer* output,
    size_t* output_len,
    std::vector<int>& output_shape,
    std::string* err
) {
    try {
        if (!predictor) {
            if (err != nullptr) {
                *err = "predictor가 없습니다";
            }
            return false;
        }
        if (output == nullptr || output_len == nullptr) {
            if (err != nullptr) {
                *err = "출력 scratch buffer가 없습니다";
            }
            return false;
        }
        const PredictorIoNames io_names = resolve_predictor_io_names(predictor);
        if (io_names.input_name.empty()) {
            if (err != nullptr) {
                *err = "모델 입력 노드가 비어 있습니다";
            }
            return false;
        }
        if (debug_enabled()) {
            std::string shape_str;
            for (size_t i = 0; i < shape.size(); ++i) {
                if (i > 0) {
                    shape_str += "x";
                }
                shape_str += std::to_string(shape[i]);
            }
            debug_log("run_predictor input_name=" + io_names.input_name +
                      ", shape=" + shape_str +
                      ", input_len=" + std::to_string(input_len));
            if (input_data != nullptr && input_len > 0) {
                debug_log("run_predictor input sample=" +
                          std::to_string(input_data[0]) + ", " +
                          std::to_string(input_data[input_len / 2]) + ", " +
                          std::to_string(input_data[input_len - 1]));
            }
        }

        auto input_handle = predictor->GetInputHandle(io_names.input_name);
        if (!input_handle) {
            if (err != nullptr) {
                *err = "입력 핸들을 확보하지 못했습니다";
            }
            return false;
        }
        input_handle->Reshape(shape);
        input_handle->CopyFromCpu(input_data);

        if (!predictor->Run()) {
            if (err != nullptr) {
                *err = "predictor 실행 실패";
            }
            return false;
        }

        if (io_names.output_name.empty()) {
            if (err != nullptr) {
                *err = "모델 출력 노드가 비어 있습니다";
            }
            return false;
        }
        if (debug_enabled()) {
            debug_log("run_predictor output_name=" + io_names.output_name);
        }

        auto output_handle = predictor->GetOutputHandle(io_names.output_name);
        if (!output_handle) {
            if (err != nullptr) {
                *err = "출력 핸들을 확보하지 못했습니다";
            }
            return false;
        }
        auto output_shape64 = output_handle->shape();
        output_shape = shape_to_ints(output_shape64);
        const size_t n = shape_elements(output_shape);
        *output_len = n;
        if (n == 0) {
            return true;
        }
        if (!output->ensure(n, err)) {
            return false;
        }
        output_handle->CopyToCpu(output->get());
        return true;
    } catch (const std::exception& ex) {
        if (err != nullptr) {
            *err = std::string("predictor 실행 예외: ") + ex.what();
        }
        return false;
    }
}

std::vector<std::vector<int>> neighbors4() {
    return {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
}

std::vector<std::vector<int>> neighbors8() {
    return {
        {-1, -1}, {0, -1}, {1, -1},
        {-1, 0},            {1, 0},
        {-1, 1},  {0, 1},   {1, 1},
    };
}

void log_det_map_stats(const std::string& prefix, const std::vector<float>& pred, int h, int w) {
    if (!debug_enabled() || pred.empty() || h <= 0 || w <= 0) {
        return;
    }
    float mn = pred[0];
    float mx = pred[0];
    double sum = 0.0;
    int positive = 0;
    for (const float value : pred) {
        mn = std::min(mn, value);
        mx = std::max(mx, value);
        sum += value;
        if (value > 0.0f) {
            ++positive;
        }
    }
    debug_log(prefix + " h=" + std::to_string(h) + ", w=" + std::to_string(w) +
              ", min=" + std::to_string(mn) + ", max=" + std::to_string(mx) +
              ", mean=" + std::to_string(sum / pred.size()) +
              ", pos_count=" + std::to_string(positive));
}

float polygon_area(const std::array<FloatPoint, 4>& pts) {
    float a = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        a += pts[i].x * pts[j].y - pts[j].x * pts[i].y;
    }
    return std::fabs(a) * 0.5f;
}

std::vector<FloatPoint> unclip(const std::array<FloatPoint, 4>& pts, float ratio) {
#if defined(BUZHIDAO_HAVE_PYCLIPPER_CLIPPER)
    double area = polygon_area(pts);
    double perimeter = 0.0;
    for (int i = 0; i < 4; ++i) {
        perimeter += point_distance(pts[i], pts[(i + 1) % 4]);
    }
    if (area > 0.0 && perimeter > 1e-6) {
        ClipperLib::Path path;
        path.reserve(pts.size());
        for (const auto& pt : pts) {
            path.push_back(ClipperLib::IntPoint{
                static_cast<ClipperLib::cInt>(pt.x),
                static_cast<ClipperLib::cInt>(pt.y)
            });
        }
        const double distance = area * ratio / perimeter;
        ClipperLib::Paths inflated;
        ClipperLib::ClipperOffset clipper_offset;
        clipper_offset.AddPath(path, ClipperLib::jtRound, ClipperLib::etClosedPolygon);
        clipper_offset.Execute(inflated, distance);
        if (!inflated.empty()) {
            size_t best_idx = 0;
            double best_area = 0.0;
            for (size_t i = 0; i < inflated.size(); ++i) {
                if (inflated[i].size() < 3) {
                    continue;
                }
                double candidate_area = 0.0;
                for (size_t j = 0; j < inflated[i].size(); ++j) {
                    const auto& a = inflated[i][j];
                    const auto& b = inflated[i][(j + 1) % inflated[i].size()];
                    candidate_area += static_cast<double>(a.X) * static_cast<double>(b.Y) -
                                      static_cast<double>(b.X) * static_cast<double>(a.Y);
                }
                candidate_area = std::fabs(candidate_area) * 0.5;
                if (candidate_area > best_area) {
                    best_area = candidate_area;
                    best_idx = i;
                }
            }
            if (best_area > 0.0) {
                std::vector<FloatPoint> poly;
                poly.reserve(inflated[best_idx].size());
                for (const auto& point : inflated[best_idx]) {
                    poly.push_back({
                        static_cast<float>(point.X),
                        static_cast<float>(point.Y),
                    });
                }
                return poly;
            }
        }
    }
#endif
    std::vector<FloatPoint> poly(pts.begin(), pts.end());
    const auto rect = min_area_rect(convex_hull(poly));
    const auto expanded = order_clockwise(rect_to_points(expand_rect(rect, ratio)));
    return std::vector<FloatPoint>(expanded.begin(), expanded.end());
}

float score_box(
    const std::vector<float>& pred,
    int pred_h,
    int pred_w,
    const std::array<FloatPoint, 4>& box,
    ScoreBoxDebug* debug = nullptr
) {
    if (pred.empty() || pred_h <= 0 || pred_w <= 0) {
        return 0.0f;
    }
    float min_x = std::numeric_limits<float>::max();
    float max_x = 0.0f;
    float min_y = std::numeric_limits<float>::max();
    float max_y = 0.0f;
    for (int i = 0; i < 4; ++i) {
        min_x = std::min(min_x, box[i].x);
        max_x = std::max(max_x, box[i].x);
        min_y = std::min(min_y, box[i].y);
        max_y = std::max(max_y, box[i].y);
    }
    const int x0 = std::max(0, static_cast<int>(std::floor(min_x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(min_y)));
    const int x1 = std::min(pred_w - 1, static_cast<int>(std::ceil(max_x)));
    const int y1 = std::min(pred_h - 1, static_cast<int>(std::ceil(max_y)));
    if (x1 < x0 || y1 < y0) {
        if (debug != nullptr) {
            *debug = {x0, y0, x1, y1, 0, 0.0};
        }
        return 0.0f;
    }
#if defined(BUZHIDAO_HAVE_OPENCV)
    cv::Mat mask = cv::Mat::zeros(y1 - y0 + 1, x1 - x0 + 1, CV_8UC1);
    std::vector<cv::Point> shifted;
    shifted.reserve(box.size());
    for (const auto& pt : box) {
        shifted.emplace_back(
            static_cast<int>(pt.x - static_cast<float>(x0)),
            static_cast<int>(pt.y - static_cast<float>(y0))
        );
    }
    const std::vector<std::vector<cv::Point>> polys{shifted};
    cv::fillPoly(mask, polys, cv::Scalar(1));

    cv::Mat pred_mat(pred_h, pred_w, CV_32FC1, const_cast<float*>(pred.data()));
    const cv::Rect roi(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    const cv::Scalar mean_score = cv::mean(pred_mat(roi), mask);
    const int count = cv::countNonZero(mask);
    const double sum = mean_score[0] * static_cast<double>(count);
    if (debug != nullptr) {
        *debug = {x0, y0, x1, y1, count, sum};
    }
    return count > 0 ? static_cast<float>(mean_score[0]) : 0.0f;
#else
    double sum = 0.0;
    int count = 0;
    for (int y = y0; y <= y1; ++y) {
        const int row = y * pred_w;
        for (int x = x0; x <= x1; ++x) {
            const FloatPoint p{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
            if (!point_in_quad(p, box)) {
                continue;
            }
            sum += pred[row + x];
            ++count;
        }
    }
    if (count == 0) {
        if (debug != nullptr) {
            *debug = {x0, y0, x1, y1, 0, 0.0};
        }
        return 0.0f;
    }
    if (debug != nullptr) {
        *debug = {x0, y0, x1, y1, count, sum};
    }
    return static_cast<float>(sum / count);
#endif
}

using IntPoint = std::pair<int, int>;

bool is_component_cell(
    const std::vector<uint8_t>& component_mask,
    int component_w,
    int component_h,
    int x,
    int y
) {
    if (x < 0 || x >= component_w || y < 0 || y >= component_h) {
        return false;
    }
    return component_mask[static_cast<size_t>(y) * component_w + x] != 0;
}

std::vector<FloatPoint> simplify_contour(const std::vector<FloatPoint>& contour) {
    if (contour.size() <= 2) {
        return contour;
    }
    std::vector<FloatPoint> simplified;
    simplified.reserve(contour.size());
    for (size_t i = 0; i < contour.size(); ++i) {
        const auto& prev = contour[(i + contour.size() - 1) % contour.size()];
        const auto& curr = contour[i];
        const auto& next = contour[(i + 1) % contour.size()];
        const float vx1 = curr.x - prev.x;
        const float vy1 = curr.y - prev.y;
        const float vx2 = next.x - curr.x;
        const float vy2 = next.y - curr.y;
        if (std::fabs(vx1 * vy2 - vy1 * vx2) < 1e-4f) {
            continue;
        }
        simplified.push_back(curr);
    }
    return simplified.empty() ? contour : simplified;
}

std::vector<FloatPoint> dedupe_contour_points(const std::vector<FloatPoint>& contour) {
    if (contour.empty()) {
        return contour;
    }
    std::vector<FloatPoint> deduped;
    deduped.reserve(contour.size());
    for (const auto& pt : contour) {
        if (!deduped.empty() &&
            std::fabs(deduped.back().x - pt.x) < 1e-4f &&
            std::fabs(deduped.back().y - pt.y) < 1e-4f) {
            continue;
        }
        deduped.push_back(pt);
    }
    if (deduped.size() > 1 &&
        std::fabs(deduped.front().x - deduped.back().x) < 1e-4f &&
        std::fabs(deduped.front().y - deduped.back().y) < 1e-4f) {
        deduped.pop_back();
    }
    return deduped;
}

std::vector<FloatPoint> normalize_contour_to_pixel_grid(
    const std::vector<FloatPoint>& contour,
    int max_x,
    int max_y
) {
    if (contour.empty()) {
        return contour;
    }
    std::vector<FloatPoint> normalized = contour;
    const float max_x_edge = static_cast<float>(max_x + 1);
    const float max_y_edge = static_cast<float>(max_y + 1);
    for (auto& pt : normalized) {
        if (std::fabs(pt.x - max_x_edge) < 1e-4f) {
            pt.x = static_cast<float>(max_x);
        }
        if (std::fabs(pt.y - max_y_edge) < 1e-4f) {
            pt.y = static_cast<float>(max_y);
        }
    }
    return dedupe_contour_points(normalized);
}

std::vector<FloatPoint> compress_dense_contour_runs(const std::vector<FloatPoint>& contour) {
    if (contour.size() <= 8) {
        return contour;
    }
    std::vector<FloatPoint> compressed;
    compressed.reserve(contour.size());
    compressed.push_back(contour.front());
    for (size_t i = 1; i + 1 < contour.size(); ++i) {
        const auto& prev = contour[i - 1];
        const auto& curr = contour[i];
        const auto& next = contour[i + 1];
        const float dx1 = curr.x - prev.x;
        const float dy1 = curr.y - prev.y;
        const float dx2 = next.x - curr.x;
        const float dy2 = next.y - curr.y;
        const bool same_direction =
            std::fabs(dx1 - dx2) < 1e-4f &&
            std::fabs(dy1 - dy2) < 1e-4f;
        if (same_direction) {
            continue;
        }
        compressed.push_back(curr);
    }
    compressed.push_back(contour.back());
    return dedupe_contour_points(compressed);
}

std::vector<FloatPoint> trace_component_contour(
    const std::vector<std::pair<int, int>>& component,
    int pred_w,
    int pred_h
) {
    if (component.empty()) {
        return {};
    }
    int min_x = pred_w;
    int min_y = pred_h;
    int max_x = 0;
    int max_y = 0;
    for (const auto& cell : component) {
        min_x = std::min(min_x, cell.first);
        min_y = std::min(min_y, cell.second);
        max_x = std::max(max_x, cell.first);
        max_y = std::max(max_y, cell.second);
    }
    const int component_w = max_x - min_x + 1;
    const int component_h = max_y - min_y + 1;
    if (component_w <= 0 || component_h <= 0) {
        return {};
    }

    std::vector<uint8_t> component_mask(static_cast<size_t>(component_w) * component_h, 0);
    for (const auto& cell : component) {
        const int lx = cell.first - min_x;
        const int ly = cell.second - min_y;
        component_mask[static_cast<size_t>(ly) * component_w + lx] = 1;
    }

    struct Edge {
        IntPoint from;
        IntPoint to;
        IntPoint cell;
    };
    std::vector<Edge> edges;
    edges.reserve(component.size() * 4);
    for (const auto& cell : component) {
        const int lx = cell.first - min_x;
        const int ly = cell.second - min_y;
        if (!is_component_cell(component_mask, component_w, component_h, lx, ly - 1)) {
            edges.push_back({{lx, ly}, {lx + 1, ly}, {lx, ly}});
        }
        if (!is_component_cell(component_mask, component_w, component_h, lx + 1, ly)) {
            edges.push_back({{lx + 1, ly}, {lx + 1, ly + 1}, {lx, ly}});
        }
        if (!is_component_cell(component_mask, component_w, component_h, lx, ly + 1)) {
            edges.push_back({{lx + 1, ly + 1}, {lx, ly + 1}, {lx, ly}});
        }
        if (!is_component_cell(component_mask, component_w, component_h, lx - 1, ly)) {
            edges.push_back({{lx, ly + 1}, {lx, ly}, {lx, ly}});
        }
    }
    if (edges.size() < 4) {
        return {};
    }

    auto dir_code = [](const Edge& edge) -> int {
        const int dx = edge.to.first - edge.from.first;
        const int dy = edge.to.second - edge.from.second;
        if (dx > 0) {
            return 0;
        }
        if (dy > 0) {
            return 1;
        }
        if (dx < 0) {
            return 2;
        }
        return 3;
    };

    std::unordered_map<int64_t, std::vector<size_t>> outgoing;
    outgoing.reserve(edges.size());
    auto point_key = [](const IntPoint& p) -> int64_t {
        return (static_cast<int64_t>(p.first) << 32) ^
               static_cast<uint32_t>(p.second);
    };
    for (size_t i = 0; i < edges.size(); ++i) {
        outgoing[point_key(edges[i].from)].push_back(i);
    }

    size_t start_idx = 0;
    for (size_t i = 1; i < edges.size(); ++i) {
        if (edges[i].from.second < edges[start_idx].from.second ||
            (edges[i].from.second == edges[start_idx].from.second &&
             edges[i].from.first < edges[start_idx].from.first) ||
            (edges[i].from == edges[start_idx].from &&
             dir_code(edges[i]) < dir_code(edges[start_idx]))) {
            start_idx = i;
        }
    }

    std::vector<uint8_t> used(edges.size(), 0);
    std::vector<FloatPoint> loop;
    loop.reserve(edges.size());
    const IntPoint start = edges[start_idx].from;
    IntPoint current = start;
    size_t current_idx = start_idx;
    int current_dir = dir_code(edges[current_idx]);

    while (true) {
        if (used[current_idx]) {
            loop.clear();
            break;
        }
        used[current_idx] = 1;
        loop.push_back({
            static_cast<float>(edges[current_idx].cell.first + min_x),
            static_cast<float>(edges[current_idx].cell.second + min_y),
        });
        current = edges[current_idx].to;
        if (current == start) {
            break;
        }

        const auto it = outgoing.find(point_key(current));
        if (it == outgoing.end() || it->second.empty()) {
            loop.clear();
            break;
        }

        size_t next_idx = edges.size();
        int best_delta = 5;
        for (size_t candidate_idx : it->second) {
            if (used[candidate_idx]) {
                continue;
            }
            const int candidate_dir = dir_code(edges[candidate_idx]);
            const int delta = (candidate_dir - current_dir + 4) % 4;
            if (delta < best_delta) {
                best_delta = delta;
                next_idx = candidate_idx;
            }
        }
        if (next_idx >= edges.size()) {
            loop.clear();
            break;
        }
        current_idx = next_idx;
        current_dir = dir_code(edges[current_idx]);
    }
    auto normalized = normalize_contour_to_pixel_grid(loop, max_x, max_y);
    if (normalized.size() > 256) {
        normalized = compress_dense_contour_runs(normalized);
    }
    return normalized;
}

std::vector<BBox> db_postprocess(
    const std::vector<float>& pred,
    int pred_h,
    int pred_w,
    int src_h,
    int src_w,
    const DetOptions& options
) {
    if (pred.empty() || pred_h <= 0 || pred_w <= 0) {
        return {};
    }
#if defined(BUZHIDAO_HAVE_OPENCV)
    cv::Mat pred_mat(pred_h, pred_w, CV_32FC1, const_cast<float*>(pred.data()));
    cv::Mat bitmap;
    cv::threshold(pred_mat, bitmap, options.threshold, 1.0, cv::THRESH_BINARY);
    bitmap.convertTo(bitmap, CV_8UC1, 255.0);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(bitmap, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);

    std::vector<BBox> boxes;
    std::vector<DetDebugCandidate> debug_candidates;
    const float x_scale = static_cast<float>(src_w) / static_cast<float>(pred_w);
    const float y_scale = static_cast<float>(src_h) / static_cast<float>(pred_h);
    const int num_contours = std::min(static_cast<int>(contours.size()), options.max_candidates);

    for (int index = 0; index < num_contours; ++index) {
        const auto& contour = contours[static_cast<size_t>(index)];
        if (contour.empty()) {
            continue;
        }
        std::vector<FloatPoint> contour_points;
        contour_points.reserve(contour.size());
        for (const auto& point : contour) {
            contour_points.push_back({
                static_cast<float>(point.x),
                static_cast<float>(point.y),
            });
        }
        const auto contour_bbox = cv::boundingRect(contour);
        const auto contour_box = min_area_rect_box_like_opencv(contour_points);
        const auto mini = get_mini_box(contour_box.corners);
        auto rect_pts = mini.first;
        const float mini_side = mini.second;
        DetDebugCandidate debug_candidate{
            static_cast<int>(contour.size()),
            contour_bbox.x,
            contour_bbox.y,
            contour_bbox.x + contour_bbox.width - 1,
            contour_bbox.y + contour_bbox.height - 1,
            {},
            {},
            {},
            contour_points,
            {
                (rect_pts[0].x + rect_pts[1].x + rect_pts[2].x + rect_pts[3].x) / 4.0f,
                (rect_pts[0].y + rect_pts[1].y + rect_pts[2].y + rect_pts[3].y) / 4.0f,
            },
            point_distance(rect_pts[0], rect_pts[1]),
            point_distance(rect_pts[1], rect_pts[2]),
            0.0f,
            rect_pts,
            rect_pts,
            {},
            {},
            0,
            0,
            0,
            0,
            0,
            0.0,
            0.0f,
            false,
            ""
        };
        if (mini_side < options.min_side) {
            debug_candidate.reject_reason = "min_side";
            debug_candidates.push_back(std::move(debug_candidate));
            continue;
        }

        ScoreBoxDebug score_debug{};
        const float score = score_box(pred, pred_h, pred_w, rect_pts, &score_debug);
        debug_candidate.score_x0 = score_debug.x0;
        debug_candidate.score_y0 = score_debug.y0;
        debug_candidate.score_x1 = score_debug.x1;
        debug_candidate.score_y1 = score_debug.y1;
        debug_candidate.score_mask_pixels = score_debug.mask_pixels;
        debug_candidate.score_sum = score_debug.sum;
        debug_candidate.score = score;
        if (score < options.box_threshold) {
            debug_candidate.reject_reason = "score";
            debug_candidates.push_back(std::move(debug_candidate));
            continue;
        }

        auto unclipped = unclip(rect_pts, options.unclip_ratio);
        const auto unclipped_box = min_area_rect_box_like_opencv(
            std::vector<FloatPoint>(unclipped.begin(), unclipped.end())
        );
        const auto unclipped_mini = get_mini_box(unclipped_box.corners);
        debug_candidate.unclipped = unclipped_mini.first;
        if (unclipped_mini.second < options.min_side + 2.0f) {
            debug_candidate.reject_reason = "unclip_side";
            debug_candidates.push_back(std::move(debug_candidate));
            continue;
        }

        std::array<FloatPoint, 4> scaled{};
        for (int i = 0; i < 4; ++i) {
            scaled[i].x = std::clamp(
                std::round(unclipped_mini.first[i].x * x_scale),
                0.0f,
                static_cast<float>(src_w)
            );
            scaled[i].y = std::clamp(
                std::round(unclipped_mini.first[i].y * y_scale),
                0.0f,
                static_cast<float>(src_h)
            );
        }
        debug_candidate.scaled = scaled;
        debug_candidate.accepted = true;
        debug_candidates.push_back(debug_candidate);
        boxes.push_back({order_clockwise(scaled), score});
        if (static_cast<int>(boxes.size()) >= options.max_candidates) {
            break;
        }
    }
    sort_quad_boxes_like_sidecar(&boxes);
    const char* dump_det_raw = std::getenv("BUZHIDAO_PADDLE_FFI_DUMP_DET");
    const bool dump_det =
        dump_det_raw != nullptr &&
        dump_det_raw[0] != '\0' &&
        std::strcmp(dump_det_raw, "0") != 0 &&
        std::strcmp(dump_det_raw, "false") != 0 &&
        std::strcmp(dump_det_raw, "FALSE") != 0;
    if (dump_det) {
        const std::string dump_dir = debug_dump_dir();
        if (!dump_dir.empty()) {
            static int det_dump_seq = 0;
            std::error_code ec;
            fs::create_directories(fs::path(dump_dir), ec);
            std::ostringstream file_name;
            file_name << "ffi_det_contours_" << det_dump_seq++
                      << "_" << src_w << "x" << src_h
                      << "_" << pred_w << "x" << pred_h
                      << ".json";
            std::ofstream ofs((fs::path(dump_dir) / file_name.str()).string(), std::ios::binary);
            if (ofs) {
                ofs << "{\n";
                ofs << "  \"src_width\": " << src_w << ",\n";
                ofs << "  \"src_height\": " << src_h << ",\n";
                ofs << "  \"pred_width\": " << pred_w << ",\n";
                ofs << "  \"pred_height\": " << pred_h << ",\n";
                ofs << "  \"items\": [\n";
                for (size_t i = 0; i < debug_candidates.size(); ++i) {
                    const auto& item = debug_candidates[i];
                    if (i > 0) {
                        ofs << ",\n";
                    }
                    ofs << "    {\n";
                    ofs << "      \"index\": " << i << ",\n";
                    ofs << "      \"component_bbox\": ["
                        << item.component_min_x << "," << item.component_min_y << ","
                        << item.component_max_x << "," << item.component_max_y << "],\n";
                    ofs << "      \"contour\": " << quote_points(item.contour) << ",\n";
                    ofs << "      \"mini_box\": " << quote_polygon(item.rect) << ",\n";
                    ofs << "      \"mini_side\": " << std::min(item.rect_width, item.rect_height) << ",\n";
                    ofs << "      \"score_bbox\": ["
                        << item.score_x0 << "," << item.score_y0 << ","
                        << item.score_x1 << "," << item.score_y1 << "],\n";
                    ofs << "      \"score_mask_pixels\": " << item.score_mask_pixels << ",\n";
                    ofs << "      \"score_sum\": " << std::setprecision(9) << item.score_sum << ",\n";
                    ofs << "      \"score\": " << std::setprecision(9) << item.score << ",\n";
                    ofs << "      \"unclipped_box\": " << quote_polygon(item.unclipped) << ",\n";
                    ofs << "      \"scaled_box\": " << quote_polygon(item.scaled) << ",\n";
                    ofs << "      \"accepted\": " << (item.accepted ? "true" : "false") << ",\n";
                    ofs << "      \"reject_reason\": \"" << json_escape(item.reject_reason) << "\"\n";
                    ofs << "    }";
                }
                ofs << "\n  ]\n";
                ofs << "}\n";
            }
        }
    }
    return boxes;
#else
    const int limit = pred_h * pred_w;
    std::vector<uint8_t> bitmap(limit, 0);
    for (int i = 0; i < limit; ++i) {
        bitmap[i] = pred[i] > options.threshold ? 1 : 0;
    }

    std::vector<uint8_t> visited(limit, 0);
    std::vector<BBox> boxes;
    std::vector<DetDebugCandidate> debug_candidates;
    const auto dirs = neighbors8();
    const float x_scale = static_cast<float>(src_w) / static_cast<float>(pred_w);
    const float y_scale = static_cast<float>(src_h) / static_cast<float>(pred_h);

    for (int y = 0; y < pred_h; ++y) {
        for (int x = 0; x < pred_w; ++x) {
            const int idx = y * pred_w + x;
            if (bitmap[idx] == 0 || visited[idx]) {
                continue;
            }

            std::queue<std::pair<int, int>> q;
            std::vector<std::pair<int, int>> component;
            visited[idx] = 1;
            q.push({x, y});

            while (!q.empty()) {
                const auto [cx, cy] = q.front();
                q.pop();
                component.push_back({cx, cy});
                for (const auto& d : dirs) {
                    const int nx = cx + d[0];
                    const int ny = cy + d[1];
                    if (nx < 0 || nx >= pred_w || ny < 0 || ny >= pred_h) {
                        continue;
                    }
                    const int ni = ny * pred_w + nx;
                    if (bitmap[ni] == 0 || visited[ni]) {
                        continue;
                    }
                    visited[ni] = 1;
                    q.push({nx, ny});
                }
            }

            const auto contour = trace_component_contour(component, pred_w, pred_h);
            int comp_min_x = pred_w;
            int comp_max_x = 0;
            int comp_min_y = pred_h;
            int comp_max_y = 0;
            for (const auto& cell : component) {
                comp_min_x = std::min(comp_min_x, cell.first);
                comp_max_x = std::max(comp_max_x, cell.first + 1);
                comp_min_y = std::min(comp_min_y, cell.second);
                comp_max_y = std::max(comp_max_y, cell.second + 1);
            }
            const int comp_w = std::max(0, comp_max_x - comp_min_x);
            const int comp_h = std::max(0, comp_max_y - comp_min_y);
            if (contour.size() < 4) {
                continue;
            }

            const auto contour_hull = convex_hull(contour);
            const auto rect = min_area_rect(contour_hull.empty() ? contour : contour_hull);
            const auto rect_points = rect_to_points(rect);
            const auto mini = get_mini_box(rect);
            const auto& rect_pts = mini.first;
            const float mini_side = mini.second;
            DetDebugCandidate debug_candidate{
                static_cast<int>(component.size()),
                comp_min_x,
                comp_min_y,
                comp_max_x,
                comp_max_y,
                {},
                {},
                {},
                contour,
                rect.center,
                rect.width,
                rect.height,
                rect.angle,
                rect_points,
                rect_pts,
                rect_pts,
                {},
                0,
                0,
                0,
                0,
                0,
                0.0,
                0.0f,
                false,
                ""
            };
            debug_candidate.component_pixels.reserve(component.size());
            for (const auto& cell : component) {
                debug_candidate.component_pixels.push_back({
                    cell.first - comp_min_x,
                    cell.second - comp_min_y,
                });
            }
            debug_candidate.component_pred.reserve(static_cast<size_t>(comp_w * comp_h));
            debug_candidate.component_bitmap.reserve(static_cast<size_t>(comp_w * comp_h));
            for (int yy = comp_min_y; yy < comp_max_y; ++yy) {
                for (int xx = comp_min_x; xx < comp_max_x; ++xx) {
                    const int pred_idx = yy * pred_w + xx;
                    debug_candidate.component_pred.push_back(pred[pred_idx]);
                    debug_candidate.component_bitmap.push_back(bitmap[pred_idx]);
                }
            }
            const bool trace_small_candidate =
                component.size() <= 256 ||
                mini_side <= 48.0f ||
                (rect.width <= 96.0f && rect.height <= 96.0f);
            if (mini_side < options.min_side) {
                debug_candidate.reject_reason = "min_side";
                debug_candidates.push_back(std::move(debug_candidate));
                if (trace_small_candidate) {
                    debug_log_lazy([&]() {
                        return "db_postprocess small reject(min_side): component=" + std::to_string(component.size()) +
                               ", comp_wh=" + std::to_string(comp_w) + "x" + std::to_string(comp_h) +
                               ", contour_n=" + std::to_string(contour.size()) +
                               ", rect_wh=" + std::to_string(rect.width) + "x" + std::to_string(rect.height) +
                               ", mini_side=" + std::to_string(mini_side) +
                               ", rect=" + quote_polygon(rect_pts) +
                               ", contour=" + quote_points(contour);
                    });
                }
                continue;
            }
            ScoreBoxDebug score_debug{};
            const float s = score_box(pred, pred_h, pred_w, rect_pts, &score_debug);
            debug_candidate.score_x0 = score_debug.x0;
            debug_candidate.score_y0 = score_debug.y0;
            debug_candidate.score_x1 = score_debug.x1;
            debug_candidate.score_y1 = score_debug.y1;
            debug_candidate.score_mask_pixels = score_debug.mask_pixels;
            debug_candidate.score_sum = score_debug.sum;
            debug_candidate.score = s;
            if (s < options.box_threshold) {
                debug_candidate.reject_reason = "score";
                debug_candidates.push_back(std::move(debug_candidate));
                if (trace_small_candidate) {
                    debug_log_lazy([&]() {
                        return "db_postprocess small reject(score): component=" + std::to_string(component.size()) +
                               ", comp_wh=" + std::to_string(comp_w) + "x" + std::to_string(comp_h) +
                               ", contour_n=" + std::to_string(contour.size()) +
                               ", rect_wh=" + std::to_string(rect.width) + "x" + std::to_string(rect.height) +
                               ", mini_side=" + std::to_string(mini_side) +
                               ", score=" + std::to_string(s) +
                               ", rect=" + quote_polygon(rect_pts) +
                               ", contour=" + quote_points(contour);
                    });
                }
                continue;
            }
            auto unclipped = unclip(rect_pts, options.unclip_ratio);
            const auto unclipped_rect = min_area_rect(std::vector<FloatPoint>(unclipped.begin(), unclipped.end()));
            const auto unclipped_mini = get_mini_box(unclipped_rect);
            const float unclipped_mini_side = unclipped_mini.second;
            debug_candidate.unclipped = unclipped_mini.first;
            if (unclipped_mini_side < options.min_side + 2.0f) {
                debug_candidate.reject_reason = "unclip_side";
                debug_candidates.push_back(std::move(debug_candidate));
                if (trace_small_candidate) {
                    debug_log_lazy([&]() {
                        return "db_postprocess small reject(unclip_side): component=" + std::to_string(component.size()) +
                               ", comp_wh=" + std::to_string(comp_w) + "x" + std::to_string(comp_h) +
                               ", contour_n=" + std::to_string(contour.size()) +
                               ", rect_wh=" + std::to_string(rect.width) + "x" + std::to_string(rect.height) +
                               ", mini_side=" + std::to_string(mini_side) +
                               ", score=" + std::to_string(s) +
                               ", rect=" + quote_polygon(rect_pts) +
                               ", unclipped=" + quote_polygon(unclipped_mini.first);
                    });
                }
                continue;
            }

            std::array<FloatPoint, 4> pts{};
            for (int i = 0; i < 4; ++i) {
                pts[i].x = std::clamp(
                    std::round(unclipped_mini.first[i].x * x_scale),
                    0.0f,
                    static_cast<float>(src_w)
                );
                pts[i].y = std::clamp(
                    std::round(unclipped_mini.first[i].y * y_scale),
                    0.0f,
                    static_cast<float>(src_h)
                );
            }
            debug_candidate.scaled = pts;
            debug_candidate.accepted = true;
            debug_candidates.push_back(std::move(debug_candidate));
            if (trace_small_candidate) {
                debug_log_lazy([&]() {
                    return "db_postprocess small accept: component=" + std::to_string(component.size()) +
                           ", comp_wh=" + std::to_string(comp_w) + "x" + std::to_string(comp_h) +
                           ", contour_n=" + std::to_string(contour.size()) +
                           ", rect_wh=" + std::to_string(rect.width) + "x" + std::to_string(rect.height) +
                           ", mini_side=" + std::to_string(mini_side) +
                           ", score=" + std::to_string(s) +
                           ", rect=" + quote_polygon(rect_pts) +
                           ", unclipped=" + quote_polygon(unclipped) +
                           ", scaled=" + quote_polygon(pts) +
                           ", contour=" + quote_points(contour);
                });
            }
            boxes.push_back({order_clockwise(pts), s});
            if (static_cast<int>(boxes.size()) >= options.max_candidates) {
                return boxes;
            }
        }
    }
    debug_log_lazy([&]() {
        return "db_postprocess: boxes=" + std::to_string(boxes.size()) +
               ", pred_h=" + std::to_string(pred_h) +
               ", pred_w=" + std::to_string(pred_w) +
               ", thresholds(" + std::to_string(options.threshold) + "/" +
               std::to_string(options.box_threshold) + "), min_side=" +
               std::to_string(options.min_side) + ", unclip=" +
               std::to_string(options.unclip_ratio);
    });
    const char* dump_det_raw = std::getenv("BUZHIDAO_PADDLE_FFI_DUMP_DET");
    const bool dump_det =
        dump_det_raw != nullptr &&
        dump_det_raw[0] != '\0' &&
        std::strcmp(dump_det_raw, "0") != 0 &&
        std::strcmp(dump_det_raw, "false") != 0 &&
        std::strcmp(dump_det_raw, "FALSE") != 0;
    if (dump_det) {
        const std::string dump_dir = debug_dump_dir();
        if (!dump_dir.empty()) {
            static int det_dump_seq = 0;
            std::error_code ec;
            fs::create_directories(fs::path(dump_dir), ec);
            std::ostringstream file_name;
            file_name << "ffi_det_contours_" << det_dump_seq++
                      << "_" << src_w << "x" << src_h
                      << "_" << pred_w << "x" << pred_h
                      << ".json";
            std::ofstream ofs((fs::path(dump_dir) / file_name.str()).string(), std::ios::binary);
            if (ofs) {
                ofs << "{\n";
                ofs << "  \"src_width\": " << src_w << ",\n";
                ofs << "  \"src_height\": " << src_h << ",\n";
                ofs << "  \"pred_width\": " << pred_w << ",\n";
                ofs << "  \"pred_height\": " << pred_h << ",\n";
                ofs << "  \"items\": [\n";
                for (size_t i = 0; i < debug_candidates.size(); ++i) {
                    const auto& item = debug_candidates[i];
                    if (i > 0) {
                        ofs << ",\n";
                    }
                    ofs << "    {\n";
                    ofs << "      \"index\": " << i << ",\n";
                    ofs << "      \"component_size\": " << item.component_size << ",\n";
                    ofs << "      \"component_bbox\": ["
                        << item.component_min_x << "," << item.component_min_y << ","
                        << item.component_max_x << "," << item.component_max_y << "],\n";
                    ofs << "      \"component_pixels\": [";
                    for (size_t j = 0; j < item.component_pixels.size(); ++j) {
                        if (j > 0) {
                            ofs << ",";
                        }
                        ofs << "[" << item.component_pixels[j].first << "," << item.component_pixels[j].second << "]";
                    }
                    ofs << "],\n";
                    ofs << "      \"component_pred\": [";
                    for (size_t j = 0; j < item.component_pred.size(); ++j) {
                        if (j > 0) {
                            ofs << ",";
                        }
                        ofs << std::setprecision(9) << item.component_pred[j];
                    }
                    ofs << "],\n";
                    ofs << "      \"component_bitmap\": [";
                    for (size_t j = 0; j < item.component_bitmap.size(); ++j) {
                        if (j > 0) {
                            ofs << ",";
                        }
                        ofs << static_cast<int>(item.component_bitmap[j]);
                    }
                    ofs << "],\n";
                    ofs << "      \"contour\": [";
                    for (size_t p = 0; p < item.contour.size(); ++p) {
                        if (p > 0) {
                            ofs << ",";
                        }
                        ofs << "[" << std::setprecision(9) << item.contour[p].x
                            << "," << std::setprecision(9) << item.contour[p].y << "]";
                    }
                    ofs << "],\n";
                    const auto dump_quad = [&](const char* key, const std::array<FloatPoint, 4>& quad) {
                        ofs << "      \"" << key << "\": [";
                        for (size_t p = 0; p < quad.size(); ++p) {
                            if (p > 0) {
                                ofs << ",";
                            }
                            ofs << "[" << std::setprecision(9) << quad[p].x
                                << "," << std::setprecision(9) << quad[p].y << "]";
                        }
                        ofs << "]";
                    };
                    ofs << "      \"min_area_rect\": {\n";
                    ofs << "        \"center\": ["
                        << std::setprecision(9) << item.rect_center.x << ","
                        << std::setprecision(9) << item.rect_center.y << "],\n";
                    ofs << "        \"width\": " << std::setprecision(9) << item.rect_width << ",\n";
                    ofs << "        \"height\": " << std::setprecision(9) << item.rect_height << ",\n";
                    ofs << "        \"angle\": " << std::setprecision(9) << item.rect_angle << "\n";
                    ofs << "      },\n";
                    dump_quad("rect_points", item.rect_points);
                    ofs << ",\n";
                    dump_quad("rect", item.rect);
                    ofs << ",\n";
                    dump_quad("unclipped", item.unclipped);
                    ofs << ",\n";
                    dump_quad("scaled", item.scaled);
                    ofs << ",\n";
                    ofs << "      \"score_bbox\": ["
                        << item.score_x0 << "," << item.score_y0 << ","
                        << item.score_x1 << "," << item.score_y1 << "],\n";
                    ofs << "      \"score_mask_pixels\": " << item.score_mask_pixels << ",\n";
                    ofs << "      \"score_sum\": " << std::setprecision(12) << item.score_sum << ",\n";
                    ofs << "      \"score\": " << std::setprecision(9) << item.score << ",\n";
                    ofs << "      \"accepted\": " << (item.accepted ? "true" : "false") << ",\n";
                    ofs << "      \"reject_reason\": \"" << json_escape(item.reject_reason) << "\"\n";
                    ofs << "    }";
                }
                ofs << "\n  ]\n";
                ofs << "}\n";
            }
        }
    }
    sort_quad_boxes_like_sidecar(&boxes);
    return boxes;
#endif
}

std::vector<BBox> run_det(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const Image& img,
    int det_resize_long,
    const ModelPreprocessCfg& det_cfg,
    const NormalizeCfg& det_norm,
    const DetOptions& det_options,
    std::string* err
) {
    const bool profile_stages = profile_stages_enabled();
    const auto det_started = std::chrono::steady_clock::now();
    double preprocess_ms = 0.0;
    double predictor_ms = 0.0;
    double postprocess_ms = 0.0;
    int resized_h = 0;
    int resized_w = 0;
    int env_det_resize_long = 0;
    const int effective_det_resize_long =
        det_resize_long > 0 ? det_resize_long
        : (parse_env_int(std::getenv("BUZHIDAO_PADDLE_FFI_DET_RESIZE_LONG"), &env_det_resize_long) &&
           env_det_resize_long > 0
           ? env_det_resize_long
           : 0);
    const int det_limit_side_len = effective_det_resize_long > 0 ? effective_det_resize_long : det_cfg.det_limit_side_len;
    const std::string det_limit_type = effective_det_resize_long > 0 ? "max" : det_cfg.det_limit_type;
    const auto preprocess_started = std::chrono::steady_clock::now();
    const auto input = preprocess_det(
        img,
        det_limit_side_len,
        det_limit_type,
        det_cfg.det_max_side_limit,
        det_norm,
        &resized_h,
        &resized_w
    );
    if (profile_stages) {
        preprocess_ms += elapsed_ms_since(preprocess_started);
    }
    if (input.empty() || resized_h == 0 || resized_w == 0) {
        if (err != nullptr) {
            *err = "det 입력 텐서를 구성할 수 없습니다";
        }
        return {};
    }
    std::vector<float> out;
    std::vector<int> shape{1, 3, resized_h, resized_w};
    std::vector<int> out_shape;
    const char* dump_det_raw = std::getenv("BUZHIDAO_PADDLE_FFI_DUMP_DET");
    const bool dump_det =
        dump_det_raw != nullptr &&
        dump_det_raw[0] != '\0' &&
        std::strcmp(dump_det_raw, "0") != 0 &&
        std::strcmp(dump_det_raw, "false") != 0 &&
        std::strcmp(dump_det_raw, "FALSE") != 0;
    if (dump_det) {
        const std::string dump_dir = debug_dump_dir();
        if (!dump_dir.empty()) {
            const Image resized = resize_for_det(
                img,
                det_limit_side_len,
                det_limit_type,
                det_cfg.det_max_side_limit,
                nullptr,
                nullptr
            );
            static int det_input_dump_seq = 0;
            std::ostringstream file_name;
            file_name << "ffi_det_input_" << det_input_dump_seq++
                      << "_" << resized.width << "x" << resized.height << ".bmp";
            std::string dump_err;
            save_bmp(fs::path(dump_dir) / file_name.str(), resized, &dump_err);
        }
    }
    const auto predictor_started = std::chrono::steady_clock::now();
    if (!run_predictor(predictor, input, shape, out, out_shape, err)) {
        set_error_if_empty(err, "det predictor 실행 실패");
        return {};
    }
    if (profile_stages) {
        predictor_ms += elapsed_ms_since(predictor_started);
    }
    if (out_shape.size() < 3) {
        if (err != nullptr) {
            *err = "det 출력 shape가 유효하지 않습니다";
        }
        return {};
    }

    int pred_h = out_shape[out_shape.size() - 2];
    int pred_w = out_shape[out_shape.size() - 1];
    if (pred_h <= 0 || pred_w <= 0) {
        if (err != nullptr) {
            *err = "det 예측 맵 크기가 0입니다";
        }
        return {};
    }
    const size_t pred_area = static_cast<size_t>(pred_h) * static_cast<size_t>(pred_w);

    if (out_shape.size() == 4) {
        const int c = out_shape[1];
        if (c > 1) {
            if (out.size() < pred_area) {
                if (err != nullptr) {
                    *err = "det 출력 길이가 예측 맵보다 짧습니다";
                }
                return {};
            }
            const auto postprocess_started = std::chrono::steady_clock::now();
            std::vector<float> single(pred_h * pred_w, 0.0f);
            for (int i = 0; i < pred_h * pred_w; ++i) {
                float best = out[i];
                for (int ch = 1; ch < c; ++ch) {
                    const size_t idx = static_cast<size_t>(ch) * pred_h * pred_w + i;
                    if (idx < out.size()) {
                        best = std::max(best, out[idx]);
                    }
                }
                single[i] = best;
            }
            ensure_probability_map(single);
            auto boxes = db_postprocess(single, pred_h, pred_w, img.height, img.width, det_options);
            if (profile_stages) {
                postprocess_ms += elapsed_ms_since(postprocess_started);
                std::ostringstream os;
                os << "run_det profile src=" << img.width << "x" << img.height
                   << ", resized=" << resized_w << "x" << resized_h
                   << ", boxes=" << boxes.size()
                   << ", preprocess_ms=" << std::fixed << std::setprecision(3) << preprocess_ms
                   << ", predictor_ms=" << predictor_ms
                   << ", postprocess_ms=" << postprocess_ms
                   << ", total_ms=" << elapsed_ms_since(det_started);
                profile_log(os.str());
                debug_log(os.str());
            }
            return boxes;
        }
    }
    debug_log_lazy([&]() {
        return std::string("run_det output_shape=") + std::to_string(out_shape.size()) + " " +
               (out_shape.size() >= 1 ? std::to_string(out_shape[0]) : std::string("x")) + "x" +
               (out_shape.size() >= 2 ? std::to_string(out_shape[1]) : std::string("")) + "x" +
               (out_shape.size() >= 3 ? std::to_string(out_shape[2]) : std::string("")) + "x" +
               (out_shape.size() >= 4 ? std::to_string(out_shape[3]) : std::string("")) +
               ", resized=" + std::to_string(resized_w) + "x" + std::to_string(resized_h) +
               ", src=" + std::to_string(img.width) + "x" + std::to_string(img.height);
    });
    if (out.size() < pred_area) {
        if (err != nullptr) {
            *err = "det 출력 길이가 예측 맵보다 짧습니다";
        }
        return {};
    }
    ensure_probability_map(out);
    log_det_map_stats("run_det pred", out, pred_h, pred_w);
    if (dump_det) {
        const std::string dump_dir = debug_dump_dir();
        if (!dump_dir.empty()) {
            static int det_dump_seq = 0;
            std::error_code ec;
            fs::create_directories(fs::path(dump_dir), ec);
            std::ostringstream file_name;
            file_name << "det_dump_" << det_dump_seq++
                      << "_" << img.width << "x" << img.height
                      << "_to_" << resized_w << "x" << resized_h
                      << "_pred_" << pred_w << "x" << pred_h
                      << ".json";
            std::ofstream ofs((fs::path(dump_dir) / file_name.str()).string(), std::ios::binary);
            if (ofs) {
                ofs << "{\n";
                ofs << "  \"image_width\": " << img.width << ",\n";
                ofs << "  \"image_height\": " << img.height << ",\n";
                ofs << "  \"input_shape\": [1,3," << resized_h << "," << resized_w << "],\n";
                ofs << "  \"pred_shape\": [1,1," << pred_h << "," << pred_w << "],\n";
                ofs << "  \"input_values\": [";
                for (size_t i = 0; i < input.size(); ++i) {
                    if (i > 0) {
                        ofs << ",";
                    }
                    ofs << std::setprecision(9) << input[i];
                }
                ofs << "],\n";
                ofs << "  \"values\": [";
                for (size_t i = 0; i < out.size(); ++i) {
                    if (i > 0) {
                        ofs << ",";
                    }
                    ofs << std::setprecision(9) << out[i];
                }
                ofs << "]\n";
                ofs << "}\n";
            } else {
                debug_log("det dump failed: open failed");
            }
        }
    }

    const auto postprocess_started = std::chrono::steady_clock::now();
    auto boxes = db_postprocess(out, pred_h, pred_w, img.height, img.width, det_options);
    if (profile_stages) {
        postprocess_ms += elapsed_ms_since(postprocess_started);
        std::ostringstream os;
        os << "run_det profile src=" << img.width << "x" << img.height
           << ", resized=" << resized_w << "x" << resized_h
           << ", boxes=" << boxes.size()
           << ", preprocess_ms=" << std::fixed << std::setprecision(3) << preprocess_ms
           << ", predictor_ms=" << predictor_ms
           << ", postprocess_ms=" << postprocess_ms
           << ", total_ms=" << elapsed_ms_since(det_started);
        profile_log(os.str());
        debug_log(os.str());
    }
    return boxes;
}

std::pair<int, float> run_cls(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const Image& img,
    const ModelPreprocessCfg& cls_cfg,
    std::string* err
) {
    const auto input = preprocess_cls(img, cls_cfg.cls_target_w, cls_cfg.cls_target_h, cls_cfg.cls_norm);
    std::vector<float> out;
    std::vector<int> shape{1, 3, cls_cfg.cls_target_h, cls_cfg.cls_target_w};
    std::vector<int> out_shape;
    if (!run_predictor(predictor, input, shape, out, out_shape, err)) {
        set_error_if_empty(err, "cls predictor 실행 실패");
        return {0, 0.0f};
    }
    if (out.empty()) {
        return {0, 0.0f};
    }
    size_t max_idx = 0;
    float max_score = out[0];
    for (size_t i = 1; i < out.size(); ++i) {
        if (out[i] > max_score) {
            max_score = out[i];
            max_idx = i;
        }
    }
    return {static_cast<int>(max_idx), max_score};
}

std::vector<std::pair<int, float>> run_cls_batch(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const std::vector<const Image*>& imgs,
    const ModelPreprocessCfg& cls_cfg,
    std::string* err
) {
    if (imgs.empty()) {
        return {};
    }
    if (imgs.size() == 1) {
        return {run_cls(predictor, *imgs[0], cls_cfg, err)};
    }

    const int batch_n = static_cast<int>(imgs.size());
    const int target_h = cls_cfg.cls_target_h;
    const int target_w = cls_cfg.cls_target_w;
    const size_t per_item = static_cast<size_t>(3 * target_h * target_w);
    std::vector<float> batch_input(static_cast<size_t>(batch_n) * per_item, 0.0f);
    for (int i = 0; i < batch_n; ++i) {
        const auto* image = imgs[static_cast<size_t>(i)];
        const Image resized = resize_bilinear(*image, target_w, target_h);
        fill_cls_tensor(
            resized,
            cls_cfg.cls_norm,
            batch_input.data() + static_cast<size_t>(i) * per_item
        );
    }

    std::vector<float> out;
    std::vector<int> out_shape;
    std::vector<int> shape{batch_n, 3, target_h, target_w};
    if (!run_predictor(predictor, batch_input, shape, out, out_shape, err)) {
        set_error_if_empty(err, "cls batch predictor 실행 실패");
        return std::vector<std::pair<int, float>>(imgs.size(), {0, 0.0f});
    }
    if (out.empty()) {
        return std::vector<std::pair<int, float>>(imgs.size(), {0, 0.0f});
    }

    int num_classes = 0;
    if (out_shape.size() == 2) {
        if (out_shape[0] != batch_n) {
            if (err != nullptr) {
                *err = "cls batch 출력 shape 파싱 실패";
            }
            return std::vector<std::pair<int, float>>(imgs.size(), {0, 0.0f});
        }
        num_classes = out_shape[1];
    } else if (out_shape.size() == 1 && batch_n == 1) {
        num_classes = out_shape[0];
    } else {
        if (err != nullptr) {
            *err = "cls batch 출력 shape 파싱 실패";
        }
        return std::vector<std::pair<int, float>>(imgs.size(), {0, 0.0f});
    }
    if (num_classes <= 0) {
        if (err != nullptr) {
            *err = "cls batch 출력 class 수가 유효하지 않습니다";
        }
        return std::vector<std::pair<int, float>>(imgs.size(), {0, 0.0f});
    }
    const size_t expected_values = static_cast<size_t>(batch_n) * static_cast<size_t>(num_classes);
    if (out.size() < expected_values) {
        if (err != nullptr) {
            *err = "cls batch 출력 길이가 shape보다 짧습니다";
        }
        return std::vector<std::pair<int, float>>(imgs.size(), {0, 0.0f});
    }

    std::vector<std::pair<int, float>> results;
    results.reserve(imgs.size());
    for (int i = 0; i < batch_n; ++i) {
        const size_t base = static_cast<size_t>(i) * static_cast<size_t>(num_classes);
        size_t max_idx = 0;
        float max_score = out[base];
        for (int c = 1; c < num_classes; ++c) {
            const float score = out[base + static_cast<size_t>(c)];
            if (score > max_score) {
                max_score = score;
                max_idx = static_cast<size_t>(c);
            }
        }
        results.push_back({static_cast<int>(max_idx), max_score});
    }
    return results;
}

int find_rec_layout(const std::vector<int>& shape, int& time_steps, int& num_classes) {
    time_steps = 0;
    num_classes = 0;
    if (shape.size() == 4) {
        // Accept layouts where the class axis is still the last axis and one inner axis is singleton.
        if (shape[2] == 1 && shape[1] > 0 && shape[3] > 0) {
            time_steps = shape[1];
            num_classes = shape[3];
            return 2; // [N,T,1,C]
        }
        if (shape[1] == 1 && shape[2] > 0 && shape[3] > 0) {
            time_steps = shape[2];
            num_classes = shape[3];
            return 2; // [N,1,T,C]
        }
    }
    if (shape.size() == 3) {
        // sidecar CTCLabelDecode uses preds.argmax(axis=-1), so class axis must be the last axis.
        time_steps = shape[1];
        num_classes = shape[2];
        return 2; // [N,T,C]
    }
    if (shape.size() == 2) {
        // [T, C]
        num_classes = shape[1];
        time_steps = shape[0];
        return 2;
    }
    return 0;
}

std::pair<std::string, float> decode_ctc(
    const float* pred,
    int time_steps,
    int num_classes,
    const std::vector<std::string>& dict
) {
    if (pred == nullptr || time_steps <= 0 || num_classes <= 0) {
        return {"", 0.0f};
    }
    int prev = -1;
    double score_sum = 0.0;
    int score_count = 0;
    std::string text;
    std::vector<int> best_indices;
    std::vector<float> best_scores;
    if (debug_enabled()) {
        best_indices.reserve(static_cast<size_t>(time_steps));
        best_scores.reserve(static_cast<size_t>(time_steps));
    }

    for (int t = 0; t < time_steps; ++t) {
        const size_t base = static_cast<size_t>(t) * num_classes;
        int best = 0;
        float best_score = pred[base];
        for (int c = 1; c < num_classes; ++c) {
            const float score = pred[base + c];
            if (score > best_score) {
                best_score = score;
                best = c;
            }
        }
        if (debug_enabled()) {
            best_indices.push_back(best);
            best_scores.push_back(best_score);
        }
        if (best != 0 && best != prev) {
            const int dict_idx = best - 1;
            if (dict_idx >= 0 && dict_idx < static_cast<int>(dict.size())) {
                text += dict[dict_idx];
                score_sum += best_score;
                ++score_count;
            }
        }
        prev = best;
    }

    const float score = score_count > 0 ? static_cast<float>(score_sum / score_count) : 0.0f;
    const char* dump_all_ctc_raw = std::getenv("BUZHIDAO_PADDLE_FFI_DEBUG_CTC_ALL");
    const bool dump_all_ctc =
        dump_all_ctc_raw != nullptr &&
        dump_all_ctc_raw[0] != '\0' &&
        std::strcmp(dump_all_ctc_raw, "0") != 0 &&
        std::strcmp(dump_all_ctc_raw, "false") != 0 &&
        std::strcmp(dump_all_ctc_raw, "FALSE") != 0;
    if (debug_enabled() && (text.empty() || dump_all_ctc)) {
        std::ostringstream os;
        os << "decode_ctc text='" << text << "' top_idx=";
        for (size_t i = 0; i < best_indices.size() && i < 24; ++i) {
            if (i > 0) {
                os << ",";
            }
            os << best_indices[i] << "@" << std::fixed << std::setprecision(4) << best_scores[i];
            if (best_indices[i] > 0) {
                const int dict_idx = best_indices[i] - 1;
                if (dict_idx >= 0 && dict_idx < static_cast<int>(dict.size())) {
                    os << ":" << dict[dict_idx];
                }
            }
        }
        profile_log(os.str());
        debug_log(os.str());
    }
    return {text, score};
}

std::pair<std::string, float> decode_ctc(
    const std::vector<float>& pred,
    int time_steps,
    int num_classes,
    const std::vector<std::string>& dict
) {
    if (pred.empty()) {
        return {"", 0.0f};
    }
    return decode_ctc(pred.data(), time_steps, num_classes, dict);
}

std::pair<std::string, float> run_rec(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const Image& img,
    const std::vector<std::string>& dict,
    const ModelPreprocessCfg& rec_cfg,
    std::string* err
) {
    int rec_w = 0;
    const Image rec_input_image = resize_rec_input_image(
        img,
        rec_cfg.rec_target_h,
        rec_cfg.rec_target_w,
        rec_cfg.rec_max_w,
        rec_w
    );
    const std::string dump_dir = debug_dump_dir();
    if (!dump_dir.empty()) {
        static int rec_dump_seq = 0;
        std::error_code ec;
        fs::create_directories(fs::path(dump_dir), ec);
        std::ostringstream file_name;
        file_name << "rec_input_" << rec_dump_seq++
                  << "_" << img.width << "x" << img.height
                  << "_to_" << rec_w << "x" << rec_input_image.height
                  << ".bmp";
        std::string dump_err;
        const Image padded_dump = pad_rec_input_image(rec_input_image, rec_w);
        if (!save_bmp(fs::path(dump_dir) / file_name.str(), padded_dump, &dump_err) && !dump_err.empty()) {
            debug_log("rec input dump failed: " + dump_err);
        }
    }
    std::vector<float> input(
        1u * 3u * static_cast<size_t>(rec_cfg.rec_target_h * rec_w),
        0.0f
    );
    fill_rec_tensor(rec_input_image, rec_cfg.rec_norm, rec_w, input.data());
    std::vector<float> out;
    std::vector<int> shape{1, 3, rec_cfg.rec_target_h, rec_w};
    std::vector<int> out_shape;
    if (!run_predictor(predictor, input, shape, out, out_shape, err)) {
        set_error_if_empty(err, "rec predictor 실행 실패");
        return {"", 0.0f};
    }
    if (out.empty()) {
        return {"", 0.0f};
    }

    int time_steps = 0;
    int num_classes = 0;
    const int layout = find_rec_layout(out_shape, time_steps, num_classes);
    if (layout <= 0 || time_steps <= 0 || num_classes <= 0) {
        if (err != nullptr) {
            *err = "rec 출력 shape 파싱 실패";
        }
        return {"", 0.0f};
    }
    if (!dict.empty() && num_classes != static_cast<int>(dict.size()) + 1) {
        debug_log_lazy([&]() {
            return "run_rec dict/model class mismatch: dict_size=" + std::to_string(dict.size()) +
                   ", num_classes=" + std::to_string(num_classes);
        });
    }
    const size_t expected_values = static_cast<size_t>(time_steps) * static_cast<size_t>(num_classes);
    if (out.size() < expected_values) {
        if (err != nullptr) {
            *err = "rec 출력 길이가 shape보다 짧습니다";
        }
        return {"", 0.0f};
    }
    if (debug_enabled()) {
        std::string shape_str;
        for (size_t i = 0; i < out_shape.size(); ++i) {
            if (i > 0) {
                shape_str += "x";
            }
            shape_str += std::to_string(out_shape[i]);
        }
        debug_log("run_rec input_resized_w=" + std::to_string(rec_w) +
                  ", input_shape=1x3x" + std::to_string(rec_cfg.rec_target_h) + "x" +
                  std::to_string(rec_w) +
                  ", base_w=" + std::to_string(rec_cfg.rec_target_w) +
                  ", max_w=" + std::to_string(rec_cfg.rec_max_w) +
                  ", output_shape=" + shape_str +
                  ", layout=" + std::to_string(layout) +
                  ", time_steps=" + std::to_string(time_steps) +
                  ", num_classes=" + std::to_string(num_classes) +
                  ", dict_size=" + std::to_string(dict.size()));
    }
    const char* dump_logits_raw = std::getenv("BUZHIDAO_PADDLE_FFI_DUMP_REC_LOGITS");
    const bool dump_logits =
        dump_logits_raw != nullptr &&
        dump_logits_raw[0] != '\0' &&
        std::strcmp(dump_logits_raw, "0") != 0 &&
        std::strcmp(dump_logits_raw, "false") != 0 &&
        std::strcmp(dump_logits_raw, "FALSE") != 0;
    if (dump_logits) {
        const std::string dump_dir = debug_dump_dir();
        if (!dump_dir.empty()) {
            static int rec_logits_dump_seq = 0;
            std::error_code ec;
            fs::create_directories(fs::path(dump_dir), ec);
            std::ostringstream file_name;
            file_name << "rec_logits_" << rec_logits_dump_seq++
                      << "_" << img.width << "x" << img.height
                      << "_ts" << time_steps
                      << "_cls" << num_classes
                      << ".json";
            std::ofstream ofs((fs::path(dump_dir) / file_name.str()).string(), std::ios::binary);
            if (ofs) {
                ofs << "{\n";
                ofs << "  \"image_width\": " << img.width << ",\n";
                ofs << "  \"image_height\": " << img.height << ",\n";
                ofs << "  \"rec_width\": " << rec_w << ",\n";
                ofs << "  \"input_shape\": [1,3," << rec_cfg.rec_target_h << "," << rec_w << "],\n";
                ofs << "  \"time_steps\": " << time_steps << ",\n";
                ofs << "  \"num_classes\": " << num_classes << ",\n";
                ofs << "  \"input_values\": [";
                for (size_t i = 0; i < input.size(); ++i) {
                    if (i > 0) {
                        ofs << ",";
                    }
                    ofs << std::setprecision(9) << input[i];
                }
                ofs << "],\n";
                ofs << "  \"values\": [";
                for (size_t i = 0; i < out.size(); ++i) {
                    if (i > 0) {
                        ofs << ",";
                    }
                    ofs << std::setprecision(9) << out[i];
                }
                ofs << "]\n";
                ofs << "}\n";
            } else {
                debug_log("rec logits dump failed: open failed");
            }
        }
    }
    return decode_ctc(out, time_steps, num_classes, dict);
}

std::vector<std::pair<std::string, float>> run_rec_batch(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const std::vector<const Image*>& imgs,
    const std::vector<std::string>& dict,
    const ModelPreprocessCfg& rec_cfg,
    const std::vector<RecDebugMeta>* debug_meta,
    RecBatchScratch* scratch,
    std::string* err
) {
    if (imgs.empty()) {
        return {};
    }
    if (imgs.size() == 1) {
        return {run_rec(predictor, *imgs[0], dict, rec_cfg, err)};
    }

    const bool profile_stages = profile_stages_enabled();
    const auto rec_started = std::chrono::steady_clock::now();
    double prepare_ms = 0.0;
    double fill_ms = 0.0;
    double predictor_ms = 0.0;
    double decode_ms = 0.0;
    std::vector<Image> prepared_inputs;
    prepared_inputs.reserve(imgs.size());
    std::vector<int> prepared_widths;
    prepared_widths.reserve(imgs.size());
    RecBatchScratch local_scratch;
    RecBatchScratch& buffers = scratch != nullptr ? *scratch : local_scratch;
    int batch_w = 0;
    const auto prepare_started = std::chrono::steady_clock::now();
    for (const auto* img : imgs) {
        int rec_w = 0;
        prepared_inputs.push_back(resize_rec_input_image(
            *img,
            rec_cfg.rec_target_h,
            rec_cfg.rec_target_w,
            rec_cfg.rec_max_w,
            rec_w
        ));
        prepared_widths.push_back(rec_w);
        batch_w = std::max(batch_w, rec_w);
    }
    if (profile_stages) {
        prepare_ms += elapsed_ms_since(prepare_started);
    }

    const int batch_n = static_cast<int>(imgs.size());
    const int rec_h = rec_cfg.rec_target_h;
    const size_t sample_stride = static_cast<size_t>(3 * rec_h * batch_w);
    const size_t batch_input_len = static_cast<size_t>(batch_n) * sample_stride;
    if (!buffers.input.ensure(batch_input_len, err)) {
        return std::vector<std::pair<std::string, float>>(imgs.size(), {"", 0.0f});
    }
    float* batch_input = buffers.input.get();
    const auto fill_started = std::chrono::steady_clock::now();
    for (int i = 0; i < batch_n; ++i) {
        fill_rec_tensor(
            prepared_inputs[static_cast<size_t>(i)],
            rec_cfg.rec_norm,
            batch_w,
            batch_input + static_cast<size_t>(i) * sample_stride
        );
    }
    if (profile_stages) {
        fill_ms += elapsed_ms_since(fill_started);
    }

    std::vector<int> shape{batch_n, 3, rec_h, batch_w};
    size_t out_len = 0;
    const auto predictor_started = std::chrono::steady_clock::now();
    if (!run_predictor_into_buffer(
            predictor,
            batch_input,
            batch_input_len,
            shape,
            &buffers.output,
            &out_len,
            buffers.output_shape,
            err)) {
        set_error_if_empty(err, "rec batch predictor 실행 실패");
        return std::vector<std::pair<std::string, float>>(imgs.size(), {"", 0.0f});
    }
    if (profile_stages) {
        predictor_ms += elapsed_ms_since(predictor_started);
    }
    if (out_len == 0) {
        return std::vector<std::pair<std::string, float>>(imgs.size(), {"", 0.0f});
    }
    const float* out = buffers.output.get();

    int time_steps = 0;
    int num_classes = 0;
    const int layout = find_rec_layout(buffers.output_shape, time_steps, num_classes);
    if (layout <= 0 || time_steps <= 0 || num_classes <= 0) {
        if (err != nullptr) {
            *err = "rec batch 출력 shape 파싱 실패";
        }
        return std::vector<std::pair<std::string, float>>(imgs.size(), {"", 0.0f});
    }
    if (!dict.empty() && num_classes != static_cast<int>(dict.size()) + 1) {
        debug_log_lazy([&]() {
            return "run_rec_batch dict/model class mismatch: dict_size=" + std::to_string(dict.size()) +
                   ", num_classes=" + std::to_string(num_classes);
        });
    }
    const size_t per_item = static_cast<size_t>(time_steps) * static_cast<size_t>(num_classes);
    const size_t expected_values = static_cast<size_t>(batch_n) * per_item;
    if (out_len < expected_values) {
        if (err != nullptr) {
            *err = "rec batch 출력 길이가 shape보다 짧습니다";
        }
        return std::vector<std::pair<std::string, float>>(imgs.size(), {"", 0.0f});
    }

    if (debug_enabled()) {
        std::ostringstream os;
        os << "run_rec_batch batch_n=" << batch_n
           << ", batch_w=" << batch_w
           << ", output_shape=";
        for (size_t i = 0; i < buffers.output_shape.size(); ++i) {
            if (i > 0) {
                os << "x";
            }
            os << buffers.output_shape[i];
        }
        os << ", layout=" << layout
           << ", time_steps=" << time_steps
           << ", num_classes=" << num_classes
           << ", widths=";
        for (size_t i = 0; i < prepared_inputs.size(); ++i) {
            if (i > 0) {
                os << ",";
            }
            os << prepared_widths[i];
        }
        debug_log(os.str());
    }

    const char* dump_logits_raw = std::getenv("BUZHIDAO_PADDLE_FFI_DUMP_REC_LOGITS");
    const bool dump_logits =
        dump_logits_raw != nullptr &&
        dump_logits_raw[0] != '\0' &&
        std::strcmp(dump_logits_raw, "0") != 0 &&
        std::strcmp(dump_logits_raw, "false") != 0 &&
        std::strcmp(dump_logits_raw, "FALSE") != 0;
    if (dump_logits) {
        const std::string dump_dir = debug_dump_dir();
        if (!dump_dir.empty()) {
            static int rec_batch_dump_seq = 0;
            std::error_code ec;
            fs::create_directories(fs::path(dump_dir), ec);
            std::ostringstream file_name;
            file_name << "rec_batch_logits_" << rec_batch_dump_seq++
                      << "_n" << batch_n
                      << "_w" << batch_w
                      << "_ts" << time_steps
                      << "_cls" << num_classes
                      << ".json";
            std::ofstream ofs((fs::path(dump_dir) / file_name.str()).string(), std::ios::binary);
            if (ofs) {
                ofs << "{\n";
                ofs << "  \"batch_n\": " << batch_n << ",\n";
                ofs << "  \"batch_w\": " << batch_w << ",\n";
                ofs << "  \"input_shape\": [" << batch_n << ",3," << rec_h << "," << batch_w << "],\n";
                ofs << "  \"output_shape\": [";
                for (size_t i = 0; i < buffers.output_shape.size(); ++i) {
                    if (i > 0) {
                        ofs << ",";
                    }
                    ofs << buffers.output_shape[i];
                }
                ofs << "],\n";
                ofs << "  \"layout\": " << layout << ",\n";
                ofs << "  \"time_steps\": " << time_steps << ",\n";
                ofs << "  \"num_classes\": " << num_classes << ",\n";
                ofs << "  \"items\": [\n";
                for (int i = 0; i < batch_n; ++i) {
                    if (i > 0) {
                        ofs << ",\n";
                    }
                    const size_t sample_base = static_cast<size_t>(i) * per_item;
                    ofs << "    {\n";
                    ofs << "      \"index\": " << i << ",\n";
                    if (debug_meta != nullptr &&
                        static_cast<size_t>(i) < debug_meta->size()) {
                        const auto& meta = (*debug_meta)[static_cast<size_t>(i)];
                        ofs << "      \"original_index\": " << meta.original_index << ",\n";
                        ofs << "      \"ratio\": " << std::setprecision(9) << meta.ratio << ",\n";
                        ofs << "      \"cls_label\": " << meta.cls_label << ",\n";
                        ofs << "      \"cls_score\": " << std::setprecision(9) << meta.cls_score << ",\n";
                        ofs << "      \"rotated_180\": " << (meta.rotated_180 ? "true" : "false") << ",\n";
                        ofs << "      \"crop_width\": " << meta.crop_width << ",\n";
                        ofs << "      \"crop_height\": " << meta.crop_height << ",\n";
                        ofs << "      \"polygon\": [";
                        for (size_t p = 0; p < meta.pts.size(); ++p) {
                            if (p > 0) {
                                ofs << ",";
                            }
                            ofs << "[" << std::setprecision(9) << meta.pts[p].x
                                << "," << std::setprecision(9) << meta.pts[p].y << "]";
                        }
                        ofs << "],\n";
                        ofs << "      \"crop_quad\": [";
                        for (size_t p = 0; p < meta.crop_quad.size(); ++p) {
                            if (p > 0) {
                                ofs << ",";
                            }
                            ofs << "[" << std::setprecision(9) << meta.crop_quad[p].x
                                << "," << std::setprecision(9) << meta.crop_quad[p].y << "]";
                        }
                        ofs << "],\n";
                    }
                    const auto* raw_img = imgs[static_cast<size_t>(i)];
                    ofs << "      \"image_width\": " << raw_img->width << ",\n";
                    ofs << "      \"image_height\": " << raw_img->height << ",\n";
                    const size_t raw_pixel_count = static_cast<size_t>(raw_img->width * raw_img->height);
                    std::array<double, 4> raw_channel_sums{0.0, 0.0, 0.0, 0.0};
                    for (size_t p = 0; p < raw_pixel_count; ++p) {
                        const size_t base = p * 4;
                        raw_channel_sums[0] += raw_img->pixels[base + 0];
                        raw_channel_sums[1] += raw_img->pixels[base + 1];
                        raw_channel_sums[2] += raw_img->pixels[base + 2];
                        raw_channel_sums[3] += raw_img->pixels[base + 3];
                    }
                    ofs << "      \"raw_channel_means\": [";
                    for (size_t c = 0; c < raw_channel_sums.size(); ++c) {
                        if (c > 0) {
                            ofs << ",";
                        }
                        const double mean = raw_pixel_count > 0
                            ? raw_channel_sums[c] / static_cast<double>(raw_pixel_count)
                            : 0.0;
                        ofs << std::setprecision(9) << mean;
                    }
                    ofs << "],\n";
                    ofs << "      \"raw_pixels\": [";
                    for (size_t j = 0; j < raw_img->pixels.size(); ++j) {
                        if (j > 0) {
                            ofs << ",";
                        }
                        ofs << static_cast<unsigned int>(raw_img->pixels[j]);
                    }
                    ofs << "],\n";
                    const auto rec_width = prepared_inputs[static_cast<size_t>(i)].width;
                    ofs << "      \"rec_width\": " << rec_width << ",\n";
                    ofs << "      \"input_values\": [";
                    const size_t item_offset = static_cast<size_t>(i) * sample_stride;
                    const size_t input_hw_stride = static_cast<size_t>(rec_h * batch_w);
                    bool first_value = true;
                    for (int c = 0; c < 3; ++c) {
                        for (int y = 0; y < rec_h; ++y) {
                            const size_t row_offset =
                                item_offset +
                                static_cast<size_t>(c) * input_hw_stride +
                                static_cast<size_t>(y) * batch_w;
                            for (int x = 0; x < rec_width; ++x) {
                                if (!first_value) {
                                    ofs << ",";
                                }
                                first_value = false;
                                ofs << std::setprecision(9)
                                    << batch_input[row_offset + static_cast<size_t>(x)];
                            }
                        }
                    }
                    ofs << "],\n";
                    ofs << "      \"values\": [";
                    for (size_t j = 0; j < per_item; ++j) {
                        if (j > 0) {
                            ofs << ",";
                        }
                        ofs << std::setprecision(9) << out[sample_base + j];
                    }
                    ofs << "]\n";
                    ofs << "    }";
                }
                ofs << "\n  ]\n";
                ofs << "}\n";
            } else {
                debug_log("rec batch logits dump failed: open failed");
            }
        }
    }

    std::vector<std::pair<std::string, float>> results;
    results.reserve(imgs.size());
    const auto decode_started = std::chrono::steady_clock::now();
    for (int i = 0; i < batch_n; ++i) {
        const size_t sample_base = static_cast<size_t>(i) * per_item;
        results.push_back(decode_ctc(out + sample_base, time_steps, num_classes, dict));
    }
    if (profile_stages) {
        decode_ms += elapsed_ms_since(decode_started);
        std::ostringstream os;
        os << "run_rec_batch profile batch_n=" << batch_n
           << ", batch_w=" << batch_w
           << ", time_steps=" << time_steps
           << ", num_classes=" << num_classes
           << ", prepare_ms=" << std::fixed << std::setprecision(3) << prepare_ms
           << ", fill_ms=" << fill_ms
           << ", predictor_ms=" << predictor_ms
           << ", decode_ms=" << decode_ms
           << ", total_ms=" << elapsed_ms_since(rec_started);
        profile_log(os.str());
        debug_log(os.str());
    }
    return results;
}

std::string quote_polygon(const std::array<FloatPoint, 4>& pts) {
    std::ostringstream s;
    s << "[";
    for (size_t i = 0; i < pts.size(); ++i) {
        if (i > 0) {
            s << ",";
        }
        s << "[" << std::fixed << std::setprecision(6) << pts[i].x << "," << pts[i].y << "]";
    }
    s << "]";
    return s.str();
}

std::string quote_points(const std::vector<FloatPoint>& pts) {
    std::ostringstream s;
    s << "[";
    for (size_t i = 0; i < pts.size(); ++i) {
        if (i > 0) {
            s << ",";
        }
        s << "[" << std::fixed << std::setprecision(3) << pts[i].x << "," << pts[i].y << "]";
    }
    s << "]";
    return s.str();
}

std::string quote_text(const std::string& text) {
    return "\"" + json_escape(text) + "\"";
}

std::vector<std::string> split_lines(const fs::path& path, std::string* err) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    if (!in) {
        if (err != nullptr) {
            *err = "사전 파일을 열 수 없습니다: " + path.string();
        }
        return {};
    }
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    if (lines.empty() && err != nullptr) {
        *err = "사전 파일이 비어있습니다: " + path.string();
    }
    return lines;
}

bool append_char_dict_from_json(const std::string& text, std::vector<std::string>& dict) {
    const std::string key = "\"character_dict\"";
    const size_t key_pos = text.find(key);
    if (key_pos == std::string::npos) {
        return false;
    }
    size_t bracket_pos = text.find('[', key_pos);
    if (bracket_pos == std::string::npos) {
        return false;
    }
    ++bracket_pos;
    int depth = 1;
    bool in_string = false;
    bool escape = false;
    std::string current;
    for (size_t i = bracket_pos; i < text.size(); ++i) {
        const char ch = text[i];
        if (in_string) {
            if (escape) {
                current.push_back(ch);
                escape = false;
            } else if (ch == '\\') {
                escape = true;
            } else if (ch == '"') {
                const std::string v = trim(current);
                if (!v.empty()) {
                    dict.push_back(v);
                }
                current.clear();
                in_string = false;
            } else {
                current.push_back(ch);
            }
        } else {
            if (ch == '"') {
                in_string = true;
            } else if (ch == '[') {
                ++depth;
            } else if (ch == ']') {
                --depth;
                if (depth == 0) {
                    return !dict.empty();
                }
            }
        }
    }
    return false;
}

bool append_char_dict_from_yaml(const std::string& text, std::vector<std::string>& dict) {
    std::istringstream stream(text);
    std::string line;
    bool in_dict = false;
    size_t dict_indent = 0;
    while (std::getline(stream, line)) {
        const auto trimmed = trim(line);
        if (!in_dict) {
            if (trimmed == "character_dict:") {
                in_dict = true;
                dict_indent = line.find_first_not_of(" \t");
                if (dict_indent == std::string::npos) {
                    dict_indent = 0;
                }
            }
            continue;
        }

        const auto indent = line.find_first_not_of(" \t");
        if (indent == std::string::npos) {
            continue;
        }
        std::string item = trim(line.substr(indent));
        if (indent <= dict_indent && (item.empty() || item[0] != '-')) {
            break;
        }
        if (item.empty() || item[0] != '-') {
            continue;
        }
        item = trim(item.substr(1));
        if (item.empty()) {
            continue;
        }
        if (item.size() >= 2) {
            const char first = item.front();
            const char last = item.back();
            if ((first == '\'' && last == '\'') || (first == '"' && last == '"')) {
                item = item.substr(1, item.size() - 2);
            }
        }
        if (item == "''''") {
            item = "'";
        } else if (item == "\\") {
            item = "\\";
        }
        dict.push_back(item);
    }
    return !dict.empty();
}

std::vector<std::string> parse_dict_from_structured_file(const fs::path& path, std::string* err) {
    std::ifstream in(path);
    if (!in) {
        if (err != nullptr) {
            *err = "사전 메타 파일을 열 수 없습니다: " + path.string();
        }
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    std::vector<std::string> dict;
    const auto ext = to_lower_ascii(path.extension().string());
    const bool parsed = (ext == ".json")
        ? append_char_dict_from_json(text, dict)
        : append_char_dict_from_yaml(text, dict);
    if (!parsed) {
        return {};
    }
    if (std::find(dict.begin(), dict.end(), std::string(" ")) == dict.end()) {
        dict.push_back(" ");
    }
    return dict;
}

fs::path find_named_submodel_dir(const fs::path& model_root, const std::string& stem) {
    const auto candidates = list_named_submodel_dirs(model_root, stem);
    if (candidates.empty()) {
        return {};
    }
    return candidates.front();
}

bool has_stem_files_in_dir(const fs::path& dir) {
    if (dir.empty()) {
        return false;
    }
    const fs::path infer_json = dir / "inference.json";
    const fs::path infer_pdiparams = dir / "inference.pdiparams";
    const fs::path infer_pdmodel = dir / "inference.pdmodel";
    try {
        return (file_exists(infer_json) && file_exists(infer_pdiparams)) ||
            (file_exists(infer_pdmodel) && file_exists(infer_pdiparams));
    } catch (const std::exception& ex) {
        debug_log(std::string("has_stem_files_in_dir failed: ") + dir.string() + ", error=" + ex.what());
        return false;
    }
}

std::pair<fs::path, fs::path> resolve_model_pair(
    const fs::path& model_root,
    const std::string& stem,
    const std::string& preferred_model_hint,
    const std::string& preferred_lang,
    const std::string& preferred_family
) {
    debug_log(
        std::string("resolve_model_pair begin: root=") + model_root.string() +
        ", stem=" + stem +
        ", preferred_model_hint=" + preferred_model_hint +
        ", preferred_lang=" + preferred_lang +
        ", preferred_family=" + preferred_family
    );
    if (model_root.empty()) {
        debug_log("resolve_model_pair: empty model_root");
        return {};
    }
    const auto resolved_token = preferred_model_hint;
    const auto resolved_lang_value = preferred_lang.empty() ? std::string("en") : preferred_lang;
    debug_log(
        std::string("resolve_model_pair args resolved: stem=") + stem +
        ", token=" + resolved_token +
        ", lang=" + resolved_lang_value
    );
    debug_log(std::string("resolve_model_pair checking root: ") + model_root.string());
    if (!directory_exists(model_root)) {
        debug_log(std::string("resolve_model_pair: model_root is not a directory: ") + model_root.string());
        return {};
    }
    debug_log(std::string("resolve_model_pair root ok: ") + model_root.string());
    try {
        debug_log(std::string("resolve_model_pair candidate search begin: ") + stem);
        const auto result = resolve_candidate_model_pair(
            model_root,
            stem,
            resolved_token,
            resolved_lang_value,
            preferred_family
        );
        debug_log(
            std::string("resolve_model_pair done: ") + stem +
            ", json=" + result.first.string() +
            ", params=" + result.second.string()
        );
        return result;
    } catch (const std::exception& ex) {
        debug_log(std::string("resolve_model_pair exception: ") + stem + ", error=" + ex.what());
        return {};
    }
}

bool parse_candidate_dict(const fs::path& candidate, std::vector<std::string>& dict) {
    if (!file_exists(candidate)) {
        return false;
    }
    if (candidate.extension() == ".txt") {
        std::string err;
        dict = split_lines(candidate, &err);
        if (!dict.empty() && std::find(dict.begin(), dict.end(), std::string(" ")) == dict.end()) {
            dict.push_back(" ");
        }
        return !dict.empty();
    }
    if (candidate.extension() == ".json" || candidate.extension() == ".yaml" ||
        candidate.extension() == ".yml") {
        std::string err;
        dict = parse_dict_from_structured_file(candidate, &err);
        return !dict.empty();
    }
    return false;
}

std::vector<std::string> load_recognition_dict(const fs::path& model_dir, std::string* err) {
    static const std::vector<std::string> direct_candidates = {
        "rec_dict.txt",
        "ppocr_keys_v1.txt",
        "ppocr_keys_v2.txt",
        "dict.txt",
        "character_dict.txt",
        "label.txt",
    };
    for (const auto& name : direct_candidates) {
        const auto path = model_dir / name;
        std::vector<std::string> dict;
        if (parse_candidate_dict(path, dict)) {
            return dict;
        }
    }

    // JSON candidates inside model directory.
    for (const auto& name : {
             "config.json",
             "inference.json",
             "inference_config.json",
             "inference.yaml",
             "inference.yml"}) {
        const auto json_path = model_dir / name;
        std::string parse_err;
        const auto dict = parse_dict_from_structured_file(json_path, &parse_err);
        if (!dict.empty()) {
            return dict;
        }
    }

    // Heuristic recursive search for likely dictionary files
    for (const auto& entry : fs::recursive_directory_iterator(model_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const fs::path p = entry.path();
        if (p.extension() != ".txt") {
            continue;
        }
        const std::string stem = p.filename().string();
        if (!is_json_like_dict_name(stem)) {
            continue;
        }
        std::vector<std::string> dict;
        if (parse_candidate_dict(p, dict)) {
            return dict;
        }
    }

    if (err != nullptr) {
        *err = RAW_DICT_HINT;
    }
    return {};
}

bool validate_recognition_dict(std::vector<std::string>* dict, std::string* err) {
    if (dict == nullptr) {
        if (err != nullptr) {
            *err = "rec_dict 버퍼가 없습니다";
        }
        return false;
    }
    std::vector<std::string> cleaned;
    cleaned.reserve(dict->size());
    bool has_non_empty = false;
    for (const auto& entry : *dict) {
        if (entry.empty()) {
            continue;
        }
        if (entry != " ") {
            has_non_empty = true;
        }
        cleaned.push_back(entry);
    }
    if (!has_non_empty) {
        if (err != nullptr) {
            *err = "rec_dict에 유효한 문자가 없습니다";
        }
        return false;
    }
    if (std::find(cleaned.begin(), cleaned.end(), std::string(" ")) == cleaned.end()) {
        cleaned.push_back(" ");
    }
    *dict = std::move(cleaned);
    return true;
}

struct PipelineOutput {
    buzhi_ocr_detection_t* detections{nullptr};
    int detection_count{0};
    int detection_capacity{0};
    buzhi_ocr_debug_detection_t* debug_detections{nullptr};
    int debug_detection_count{0};
    int debug_detection_capacity{0};
};

void reserve_pipeline_output(
    PipelineOutput* output,
    size_t detection_capacity,
    bool include_debug,
    size_t debug_capacity
) {
    if (output == nullptr) {
        return;
    }
    if (detection_capacity > static_cast<size_t>(std::numeric_limits<int>::max())) {
        detection_capacity = static_cast<size_t>(std::numeric_limits<int>::max());
    }
    if (debug_capacity > static_cast<size_t>(std::numeric_limits<int>::max())) {
        debug_capacity = static_cast<size_t>(std::numeric_limits<int>::max());
    }
    if (detection_capacity > 0) {
        output->detections = new (std::nothrow) buzhi_ocr_detection_t[detection_capacity]();
        if (output->detections == nullptr) {
            output->detection_capacity = 0;
            return;
        }
        output->detection_capacity = static_cast<int>(detection_capacity);
    }
    if (include_debug && debug_capacity > 0) {
        output->debug_detections = new (std::nothrow) buzhi_ocr_debug_detection_t[debug_capacity]();
        if (output->debug_detections == nullptr) {
            output->debug_detection_capacity = 0;
            return;
        }
        output->debug_detection_capacity = static_cast<int>(debug_capacity);
    }
}

bool append_detection(PipelineOutput* output, const std::array<FloatPoint, 4>& polygon, const std::string& text) {
    if (output == nullptr || output->detection_count >= output->detection_capacity) {
        return false;
    }
    auto& detection = output->detections[output->detection_count++];
    for (int p = 0; p < 4; ++p) {
        detection.polygon[p].x = polygon[p].x;
        detection.polygon[p].y = polygon[p].y;
    }
    detection.text = dup_string(text);
    if (detection.text == nullptr) {
        --output->detection_count;
        return false;
    }
    return true;
}

bool append_debug_detection(
    PipelineOutput* output,
    const std::array<FloatPoint, 4>& polygon,
    const std::string& text,
    float score,
    bool accepted
) {
    if (output == nullptr || output->debug_detection_count >= output->debug_detection_capacity) {
        return false;
    }
    auto& detection = output->debug_detections[output->debug_detection_count++];
    for (int p = 0; p < 4; ++p) {
        detection.polygon[p].x = polygon[p].x;
        detection.polygon[p].y = polygon[p].y;
    }
    detection.text = dup_string(text);
    if (detection.text == nullptr) {
        --output->debug_detection_count;
        return false;
    }
    detection.score = score;
    detection.accepted = accepted ? 1 : 0;
    return true;
}

void reset_pipeline_output(PipelineOutput* output) {
    if (output == nullptr) {
        return;
    }
    output->detections = nullptr;
    output->detection_count = 0;
    output->detection_capacity = 0;
    output->debug_detections = nullptr;
    output->debug_detection_count = 0;
    output->debug_detection_capacity = 0;
}

void free_pipeline_output(PipelineOutput* output) {
    if (output == nullptr) {
        return;
    }
    if (output->detections != nullptr) {
        for (int i = 0; i < output->detection_count; ++i) {
            delete[] output->detections[i].text;
            output->detections[i].text = nullptr;
        }
        delete[] output->detections;
    }
    if (output->debug_detections != nullptr) {
        for (int i = 0; i < output->debug_detection_count; ++i) {
            delete[] output->debug_detections[i].text;
            output->debug_detections[i].text = nullptr;
        }
        delete[] output->debug_detections;
    }
    reset_pipeline_output(output);
}

std::string serialize_pipeline_output_json(const PipelineOutput& output, bool include_debug) {
    std::ostringstream os;
    os << "{\"detections\":[";
    for (int i = 0; i < output.detection_count; ++i) {
        if (i > 0) {
            os << ",";
        }
        os << "{\"polygon\":[";
        for (int p = 0; p < 4; ++p) {
            if (p > 0) {
                os << ",";
            }
            os << "[" << std::fixed << std::setprecision(6)
               << output.detections[i].polygon[p].x << ","
               << output.detections[i].polygon[p].y << "]";
        }
        os << "],\"text\":\""
           << json_escape(output.detections[i].text != nullptr ? output.detections[i].text : "")
           << "\"}";
    }
    os << "]";
    if (include_debug && output.debug_detections != nullptr) {
        os << ",\"debug_detections\":[";
        for (int i = 0; i < output.debug_detection_count; ++i) {
            const auto& d = output.debug_detections[i];
            if (i > 0) {
                os << ",";
            }
            os << "{\"polygon\":[";
            for (int p = 0; p < 4; ++p) {
                if (p > 0) {
                    os << ",";
                }
                os << "[" << std::fixed << std::setprecision(6)
                   << d.polygon[p].x << "," << d.polygon[p].y << "]";
            }
            os << "],\"text\":\"" << json_escape(d.text != nullptr ? d.text : "")
               << "\",\"score\":" << std::fixed << std::setprecision(6)
               << d.score
               << ",\"accepted\":" << (d.accepted != 0 ? "true" : "false")
               << "}";
        }
        os << "]";
    }
    os << "}";
    return os.str();
}

bool collect_cls_inputs(
    const Image& img,
    const std::vector<BBox>& boxes,
    bool need_rec_debug_meta,
    double* crop_ms,
    std::vector<ClsPreparedInput>* cls_inputs,
    char** err
#if defined(BUZHIDAO_HAVE_OPENCV)
    ,
    const cv::Mat& original_bgr
#endif
) {
    if (cls_inputs == nullptr) {
        set_error(err, "OCR cls 입력 버퍼가 없습니다");
        return false;
    }
    const bool profile_stages = profile_stages_enabled();
    size_t skipped_crops = 0;
    cls_inputs->clear();
    cls_inputs->reserve(boxes.size());
    for (const auto& b : boxes) {
        std::string crop_err;
        const auto crop_started = std::chrono::steady_clock::now();
#if defined(BUZHIDAO_HAVE_OPENCV)
        Image cropped = crop_to_bbox(original_bgr, b.pts, &crop_err);
#else
        Image cropped = crop_to_bbox(img, b.pts, &crop_err);
#endif
        if (profile_stages && crop_ms != nullptr) {
            *crop_ms += elapsed_ms_since(crop_started);
        }
        if (cropped.width <= 0 || cropped.height <= 0) {
            ++skipped_crops;
            if (!crop_err.empty()) {
                debug_log_lazy([&]() { return "collect_cls_inputs crop skipped: " + crop_err; });
            }
            continue;
        }
        std::array<FloatPoint, 4> crop_quad{};
        if (need_rec_debug_meta) {
            const auto [described_crop_quad, _expected_crop_w, _expected_crop_h] = describe_crop_to_bbox(b.pts);
            crop_quad = described_crop_quad;
        }
        cls_inputs->push_back({std::move(cropped), b.pts, crop_quad});
    }
    if (!boxes.empty() && cls_inputs->empty()) {
        set_error(err, "det box는 있지만 유효한 crop을 하나도 만들지 못했습니다");
        return false;
    }
    if (skipped_crops > 0) {
        debug_log_lazy([&]() {
            return "collect_cls_inputs skipped_crops=" + std::to_string(skipped_crops) +
                   "/" + std::to_string(boxes.size());
        });
    }
    return true;
}

bool run_cls_batches_into(
    buzhi_ocr_engine* engine,
    const std::vector<ClsPreparedInput>& cls_inputs,
    double* cls_ms,
    std::vector<std::pair<int, float>>* cls_results,
    char** err
) {
    if (cls_results == nullptr) {
        set_error(err, "OCR cls 결과 버퍼가 없습니다");
        return false;
    }
    const bool profile_stages = profile_stages_enabled();
    cls_results->assign(cls_inputs.size(), {0, 0.0f});
    constexpr size_t kClsBatchSize = 6;
    for (size_t start = 0; start < cls_inputs.size(); start += kClsBatchSize) {
        const size_t end = std::min(cls_inputs.size(), start + kClsBatchSize);
        std::vector<const Image*> batch_images;
        batch_images.reserve(end - start);
        for (size_t i = start; i < end; ++i) {
            batch_images.push_back(&cls_inputs[i].cropped);
        }
        std::string cls_err;
        const auto cls_started = std::chrono::steady_clock::now();
        const auto batch_results = run_cls_batch(engine->cls_predictor, batch_images, engine->cls_cfg, &cls_err);
        if (profile_stages && cls_ms != nullptr) {
            *cls_ms += elapsed_ms_since(cls_started);
        }
        if (!cls_err.empty()) {
            set_error(err, cls_err);
            return false;
        }
        if (batch_results.size() != end - start) {
            set_error(err, "cls batch 결과 개수가 입력 수와 다릅니다");
            return false;
        }
        for (size_t i = start; i < end; ++i) {
            (*cls_results)[i] = batch_results[i - start];
        }
    }
    return true;
}

bool build_rec_candidates(
    std::vector<ClsPreparedInput>* cls_inputs,
    const std::vector<std::pair<int, float>>& cls_results,
    int debug_trace,
    double* rotate_ms,
    size_t* rotated_count,
    std::vector<RecCandidate>* rec_candidates,
    char** err
) {
    if (cls_inputs == nullptr || rec_candidates == nullptr) {
        set_error(err, "OCR rec 후보 버퍼가 없습니다");
        return false;
    }
    if (cls_results.size() != cls_inputs->size()) {
        set_error(err, "cls 결과 개수가 crop 입력 수와 다릅니다");
        return false;
    }
    const bool profile_stages = profile_stages_enabled();
    rec_candidates->clear();
    rec_candidates->reserve(cls_inputs->size());
    for (size_t i = 0; i < cls_inputs->size(); ++i) {
        auto& prepared = (*cls_inputs)[i];
        Image cropped = std::move(prepared.cropped);
        const auto cls = cls_results[i];
        if (debug_trace) {
            debug_log_lazy([&]() {
                return "cls label=" + std::to_string(cls.first) +
                       ", score=" + std::to_string(cls.second) +
                       ", crop=" + std::to_string(cropped.width) + "x" +
                       std::to_string(cropped.height);
            });
        }
        const bool rotated_180 = cls.first == 1;
        if (rotated_180) {
            const auto rotate_started = std::chrono::steady_clock::now();
            cropped = rotate180(cropped);
            if (profile_stages && rotate_ms != nullptr) {
                *rotate_ms += elapsed_ms_since(rotate_started);
            }
            if (rotated_count != nullptr) {
                ++(*rotated_count);
            }
        }
        const float ratio = static_cast<float>(cropped.width) / static_cast<float>(std::max(1, cropped.height));
        rec_candidates->push_back({
            prepared.pts,
            prepared.crop_quad,
            std::move(cropped),
            ratio,
            cls.first,
            cls.second,
            rotated_180
        });
    }
    return true;
}

std::vector<size_t> build_rec_order(const std::vector<RecCandidate>& rec_candidates) {
    std::vector<size_t> rec_order(rec_candidates.size());
    for (size_t i = 0; i < rec_order.size(); ++i) {
        rec_order[i] = i;
    }
    std::stable_sort(rec_order.begin(), rec_order.end(), [&](size_t lhs, size_t rhs) {
        return rec_candidates[lhs].ratio < rec_candidates[rhs].ratio;
    });
    return rec_order;
}

void log_rec_order(const std::vector<RecCandidate>& rec_candidates, const std::vector<size_t>& rec_order) {
    if (!debug_enabled()) {
        return;
    }
    std::ostringstream os;
    os << "run_pipeline rec_order=";
    for (size_t i = 0; i < rec_order.size(); ++i) {
        if (i > 0) {
            os << " | ";
        }
        const auto& candidate = rec_candidates[rec_order[i]];
        os << "#" << rec_order[i]
           << ":" << candidate.cropped.width << "x" << candidate.cropped.height
           << "@r=" << std::fixed << std::setprecision(4) << candidate.ratio;
    }
    debug_log(os.str());
}

void dump_rec_candidates_if_requested(
    const std::string& dump_dir,
    const Image& img,
    const std::vector<RecCandidate>& rec_candidates,
    const std::vector<size_t>& rec_order
) {
    if (dump_dir.empty()) {
        return;
    }
    static int rec_candidate_dump_seq = 0;
    std::error_code ec;
    fs::create_directories(fs::path(dump_dir), ec);
    std::ostringstream file_name;
    file_name << "ffi_rec_candidates_" << rec_candidate_dump_seq++
              << "_" << img.width << "x" << img.height << ".json";
    std::ofstream ofs((fs::path(dump_dir) / file_name.str()).string(), std::ios::binary);
    if (!ofs) {
        return;
    }
    ofs << "{\n";
    ofs << "  \"image_width\": " << img.width << ",\n";
    ofs << "  \"image_height\": " << img.height << ",\n";
    ofs << "  \"items\": [\n";
    for (size_t i = 0; i < rec_candidates.size(); ++i) {
        if (i > 0) {
            ofs << ",\n";
        }
        const auto& candidate = rec_candidates[i];
        ofs << "    {\n";
        ofs << "      \"original_index\": " << i << ",\n";
        ofs << "      \"sorted_index\": ";
        auto it = std::find(rec_order.begin(), rec_order.end(), i);
        ofs << (it == rec_order.end() ? -1 : static_cast<int>(std::distance(rec_order.begin(), it))) << ",\n";
        ofs << "      \"ratio\": " << std::setprecision(9) << candidate.ratio << ",\n";
        ofs << "      \"crop_width\": " << candidate.cropped.width << ",\n";
        ofs << "      \"crop_height\": " << candidate.cropped.height << ",\n";
        ofs << "      \"cls_label\": " << candidate.cls_label << ",\n";
        ofs << "      \"cls_score\": " << std::setprecision(9) << candidate.cls_score << ",\n";
        ofs << "      \"rotated_180\": " << (candidate.rotated_180 ? "true" : "false") << ",\n";
        ofs << "      \"crop_quad\": [";
        for (size_t p = 0; p < candidate.crop_quad.size(); ++p) {
            if (p > 0) {
                ofs << ",";
            }
            ofs << "[" << std::setprecision(9) << candidate.crop_quad[p].x
                << "," << std::setprecision(9) << candidate.crop_quad[p].y << "]";
        }
        ofs << "],\n";
        ofs << "      \"polygon\": [";
        for (size_t p = 0; p < candidate.pts.size(); ++p) {
            if (p > 0) {
                ofs << ",";
            }
            ofs << "[" << std::setprecision(9) << candidate.pts[p].x
                << "," << std::setprecision(9) << candidate.pts[p].y << "]";
        }
        ofs << "]\n";
        ofs << "    }";
    }
    ofs << "\n  ]\n";
    ofs << "}\n";
}

std::vector<std::pair<size_t, size_t>> plan_rec_batches(
    const std::vector<RecCandidate>& rec_candidates,
    const std::vector<size_t>& rec_order,
    const ModelPreprocessCfg& rec_cfg
) {
    std::vector<std::pair<size_t, size_t>> rec_batches;
    rec_batches.reserve((rec_order.size() + 5) / 6);
    constexpr size_t kRecBatchSize = 6;
    int width_budget = 0;
    parse_env_int(std::getenv("BUZHIDAO_PADDLE_FFI_REC_BATCH_WIDTH_BUDGET"), &width_budget);
    size_t batch_start = 0;
    while (batch_start < rec_order.size()) {
        if (width_budget > 0) {
            size_t batch_end = batch_start;
            int batch_max_w = 0;
            while (batch_end < rec_order.size() && batch_end - batch_start < kRecBatchSize) {
                const auto& candidate = rec_candidates[rec_order[batch_end]];
                const int estimated_w = estimate_rec_input_width(
                    candidate.cropped.width,
                    candidate.cropped.height,
                    rec_cfg.rec_target_h,
                    rec_cfg.rec_target_w,
                    rec_cfg.rec_max_w
                );
                const int next_max_w = std::max(batch_max_w, estimated_w);
                const size_t next_count = batch_end - batch_start + 1;
                if (batch_end > batch_start &&
                    next_max_w * static_cast<int>(next_count) > width_budget) {
                    break;
                }
                batch_max_w = next_max_w;
                ++batch_end;
            }
            rec_batches.push_back({batch_start, batch_end});
            batch_start = batch_end;
            continue;
        }
        const size_t batch_end = std::min(batch_start + kRecBatchSize, rec_order.size());
        rec_batches.push_back({batch_start, batch_end});
        batch_start = batch_end;
    }
    return rec_batches;
}

void log_rec_batches(
    const std::vector<RecCandidate>& rec_candidates,
    const std::vector<size_t>& rec_order,
    const std::vector<std::pair<size_t, size_t>>& rec_batches,
    const ModelPreprocessCfg& rec_cfg
) {
    if (!debug_enabled()) {
        return;
    }
    std::ostringstream os;
    os << "run_pipeline rec_batches=";
    for (size_t i = 0; i < rec_batches.size(); ++i) {
        if (i > 0) {
            os << " | ";
        }
        const auto [start, end] = rec_batches[i];
        int batch_max_w = 0;
        for (size_t pos = start; pos < end; ++pos) {
            const auto& candidate = rec_candidates[rec_order[pos]];
            batch_max_w = std::max(
                batch_max_w,
                estimate_rec_input_width(
                    candidate.cropped.width,
                    candidate.cropped.height,
                    rec_cfg.rec_target_h,
                    rec_cfg.rec_target_w,
                    rec_cfg.rec_max_w
                )
            );
        }
        os << "#" << i << ":" << (end - start) << "@" << batch_max_w;
    }
    debug_log(os.str());
}

bool run_rec_batches_into(
    buzhi_ocr_engine* engine,
    const std::vector<RecCandidate>& rec_candidates,
    const std::vector<size_t>& rec_order,
    const std::vector<std::pair<size_t, size_t>>& rec_batches,
    bool need_rec_debug_meta,
    std::vector<std::pair<std::string, float>>* rec_results,
    double* rec_ms,
    char** err
) {
    if (rec_results == nullptr) {
        set_error(err, "OCR rec 결과 버퍼가 없습니다");
        return false;
    }
    const bool profile_stages = profile_stages_enabled();
    RecBatchScratch scratch;
    for (const auto& batch : rec_batches) {
        const size_t start = batch.first;
        const size_t end = batch.second;
        std::vector<const Image*> batch_images;
        batch_images.reserve(end - start);
        std::vector<RecDebugMeta> batch_meta;
        if (need_rec_debug_meta) {
            batch_meta.reserve(end - start);
        }
        for (size_t pos = start; pos < end; ++pos) {
            const size_t original_index = rec_order[pos];
            const auto& candidate = rec_candidates[original_index];
            batch_images.push_back(&candidate.cropped);
            if (need_rec_debug_meta) {
                batch_meta.push_back({
                    original_index,
                    candidate.pts,
                    candidate.crop_quad,
                    candidate.ratio,
                    candidate.cls_label,
                    candidate.cls_score,
                    candidate.rotated_180,
                    candidate.cropped.width,
                    candidate.cropped.height
                });
            }
        }
        std::string rec_err;
        const auto rec_started = std::chrono::steady_clock::now();
        const auto batch_results = run_rec_batch(
            engine->rec_predictor,
            batch_images,
            engine->rec_dict,
            engine->rec_cfg,
            need_rec_debug_meta ? &batch_meta : nullptr,
            &scratch,
            &rec_err
        );
        if (profile_stages && rec_ms != nullptr) {
            *rec_ms += elapsed_ms_since(rec_started);
        }
        if (!rec_err.empty()) {
            set_error(err, rec_err);
            return false;
        }
        if (batch_results.size() != end - start) {
            set_error(err, "rec batch 결과 개수가 입력 수와 다릅니다");
            return false;
        }
        for (size_t pos = start; pos < end; ++pos) {
            (*rec_results)[rec_order[pos]] = batch_results[pos - start];
        }
    }
    return true;
}

void dump_candidate_crop_if_requested(const RecCandidate& candidate, const char* tag, const Image& image) {
    const std::string dump_dir = debug_dump_dir();
    if (dump_dir.empty()) {
        return;
    }
    std::error_code ec;
    fs::create_directories(fs::path(dump_dir), ec);
    std::ostringstream file_name;
    file_name << tag << "_" << static_cast<int>(std::round(candidate.pts[0].x))
              << "_" << static_cast<int>(std::round(candidate.pts[0].y))
              << "_" << image.width << "x" << image.height << ".bmp";
    std::string dump_err;
    if (!save_bmp(fs::path(dump_dir) / file_name.str(), image, &dump_err) && !dump_err.empty()) {
        debug_log(std::string(tag) + " crop dump failed: " + dump_err);
    }
}

bool append_pipeline_results(
    PipelineOutput* output,
    const std::vector<RecCandidate>& rec_candidates,
    const std::vector<std::pair<std::string, float>>& rec_results,
    float score_thresh,
    int debug_trace,
    char** err
) {
    if (output == nullptr) {
        set_error(err, "OCR 결과 버퍼가 없습니다");
        return false;
    }
    for (size_t i = 0; i < rec_candidates.size(); ++i) {
        const auto& candidate = rec_candidates[i];
        const auto& rec = rec_results[i];
        const std::string& text = rec.first;
        const float score = rec.second;
        if (debug_trace) {
            dump_candidate_crop_if_requested(candidate, "first_stage", candidate.cropped);
        }
        if (score >= score_thresh) {
            if (!append_detection(output, candidate.pts, text)) {
                set_error(err, "OCR 결과 버퍼가 부족합니다");
                return false;
            }
            if (debug_trace && !append_debug_detection(output, candidate.pts, text, score, true)) {
                set_error(err, "OCR 디버그 결과 버퍼가 부족합니다");
                return false;
            }
            if (debug_trace) {
                debug_log_lazy([&]() {
                    return "box score=" + std::to_string(score) +
                           ", text='" + text + "', accepted=true";
                });
                if (score < 0.5f) {
                    dump_candidate_crop_if_requested(candidate, "low_score", candidate.cropped);
                }
            }
        } else if (debug_trace) {
            if (!append_debug_detection(output, candidate.pts, text, score, false)) {
                set_error(err, "OCR 디버그 결과 버퍼가 부족합니다");
                return false;
            }
            debug_log_lazy([&]() {
                return "box score=" + std::to_string(score) +
                       ", text='" + text + "', accepted=false, crop=" +
                       std::to_string(candidate.cropped.width) + "x" + std::to_string(candidate.cropped.height) +
                       ", polygon=" + quote_polygon(candidate.pts);
            });
            dump_candidate_crop_if_requested(candidate, "reject", candidate.cropped);
        }
    }
    return true;
}

buzhi_ocr_result_t* build_native_result(PipelineOutput&& output, bool include_debug) {
    auto* result = new buzhi_ocr_result_t{};
    result->detections = output.detections;
    result->detection_count = output.detection_count;
    if (include_debug) {
        result->debug_detections = output.debug_detections;
        result->debug_detection_count = output.debug_detection_count;
    } else if (output.debug_detections != nullptr) {
        for (int i = 0; i < output.debug_detection_count; ++i) {
            delete[] output.debug_detections[i].text;
        }
        delete[] output.debug_detections;
    }
    reset_pipeline_output(&output);
    return result;
}

PipelineOutput run_pipeline(
    buzhi_ocr_engine* engine,
    const Image& img,
    const std::string& image_label,
    int det_resize_long,
    float score_thresh,
    int debug_trace,
    char** err
) {
    const bool profile_stages = profile_stages_enabled();
    const auto pipeline_started = std::chrono::steady_clock::now();
    double load_ms = 0.0;
    double det_ms = 0.0;
    double crop_ms = 0.0;
    double cls_ms = 0.0;
    double rotate_ms = 0.0;
    double rec_ms = 0.0;
    double post_ms = 0.0;
    size_t rotated_count = 0;
    const char* dump_rec_logits_raw = std::getenv("BUZHIDAO_PADDLE_FFI_DUMP_REC_LOGITS");
    const bool dump_rec_logits =
        dump_rec_logits_raw != nullptr &&
        dump_rec_logits_raw[0] != '\0' &&
        std::strcmp(dump_rec_logits_raw, "0") != 0 &&
        std::strcmp(dump_rec_logits_raw, "false") != 0 &&
        std::strcmp(dump_rec_logits_raw, "FALSE") != 0;
    const bool need_rec_debug_meta = debug_trace || dump_rec_logits;

#if defined(BUZHIDAO_HAVE_OPENCV)
    const auto bgr_started = std::chrono::steady_clock::now();
    cv::Mat original_bgr = image_to_cv_mat_bgr(img);
    if (original_bgr.empty()) {
        set_error(err, "OpenCV BGR 이미지 변환에 실패했습니다");
        return {};
    }
    if (profile_stages) {
        crop_ms += elapsed_ms_since(bgr_started);
    }
#endif

    if (engine->rec_dict.empty()) {
        set_error(err, "recognition 사전이 없습니다. 모델 폴더에 사전(rec_dict.txt 등)을 넣어주세요.");
        return {};
    }

    std::string det_err;
    const auto det_started = std::chrono::steady_clock::now();
    const auto boxes = run_det(
        engine->det_predictor,
        img,
        det_resize_long,
        engine->det_cfg,
        engine->det_cfg.det_norm,
        engine->det_options,
        &det_err
    );
    if (profile_stages) {
        det_ms += elapsed_ms_since(det_started);
    }
    debug_log_lazy([&]() {
        return "run_pipeline: det_boxes=" + std::to_string(boxes.size());
    });
    if (!det_err.empty()) {
        set_error(err, det_err);
        return {};
    }
    PipelineOutput output;
    std::vector<RecCandidate> rec_candidates;
    rec_candidates.reserve(boxes.size());
    reserve_pipeline_output(&output, boxes.size(), debug_trace != 0, boxes.size());
    std::vector<ClsPreparedInput> cls_inputs;
    if (!collect_cls_inputs(
            img,
            boxes,
            need_rec_debug_meta,
            &crop_ms,
            &cls_inputs,
            err
#if defined(BUZHIDAO_HAVE_OPENCV)
            ,
            original_bgr
#endif
        )) {
        free_pipeline_output(&output);
        return {};
    }
    std::vector<std::pair<int, float>> cls_results;
    if (!run_cls_batches_into(engine, cls_inputs, &cls_ms, &cls_results, err)) {
        free_pipeline_output(&output);
        return {};
    }
    if (!build_rec_candidates(
            &cls_inputs,
            cls_results,
            debug_trace,
            &rotate_ms,
            &rotated_count,
            &rec_candidates,
            err)) {
        free_pipeline_output(&output);
        return {};
    }

    const auto rec_order = build_rec_order(rec_candidates);
    if (debug_trace) {
        dump_rec_candidates_if_requested(debug_dump_dir(), img, rec_candidates, rec_order);
    }
    log_rec_order(rec_candidates, rec_order);
    const auto rec_batches = plan_rec_batches(rec_candidates, rec_order, engine->rec_cfg);
    log_rec_batches(rec_candidates, rec_order, rec_batches, engine->rec_cfg);

    std::vector<std::pair<std::string, float>> rec_results(rec_candidates.size(), {"", 0.0f});
    if (!run_rec_batches_into(
            engine,
            rec_candidates,
            rec_order,
            rec_batches,
            need_rec_debug_meta,
            &rec_results,
            &rec_ms,
            err)) {
        free_pipeline_output(&output);
        return {};
    }

    const auto post_started = std::chrono::steady_clock::now();
    if (!append_pipeline_results(&output, rec_candidates, rec_results, score_thresh, debug_trace, err)) {
        free_pipeline_output(&output);
        return {};
    }
    if (profile_stages) {
        post_ms += elapsed_ms_since(post_started);
        std::ostringstream os;
        os << "run_pipeline profile image=" << image_label
           << ", boxes=" << boxes.size()
           << ", cls_inputs=" << cls_inputs.size()
           << ", rec_candidates=" << rec_candidates.size()
           << ", rotated=" << rotated_count
           << ", load_ms=" << std::fixed << std::setprecision(3) << load_ms
           << ", det_ms=" << det_ms
           << ", crop_ms=" << crop_ms
           << ", cls_ms=" << cls_ms
           << ", rotate_ms=" << rotate_ms
           << ", rec_ms=" << rec_ms
           << ", post_ms=" << post_ms
           << ", total_ms=" << elapsed_ms_since(pipeline_started);
        profile_log(os.str());
    }
    debug_log_lazy([&]() {
        return "run_pipeline: final_detections=" + std::to_string(output.detection_count) +
               ", requested_threshold=" + std::to_string(score_thresh);
    });
    return output;
}

PipelineOutput run_pipeline_from_path(
    buzhi_ocr_engine* engine,
    const fs::path& image_path,
    int det_resize_long,
    float score_thresh,
    int debug_trace,
    char** err
) {
    const bool profile_stages = profile_stages_enabled();
    const auto pipeline_started = std::chrono::steady_clock::now();
    double load_ms = 0.0;
    double det_ms = 0.0;
    double crop_ms = 0.0;
    double cls_ms = 0.0;
    double rotate_ms = 0.0;
    double rec_ms = 0.0;
    double post_ms = 0.0;
    size_t rotated_count = 0;
    const char* dump_rec_logits_raw = std::getenv("BUZHIDAO_PADDLE_FFI_DUMP_REC_LOGITS");
    const bool dump_rec_logits =
        dump_rec_logits_raw != nullptr &&
        dump_rec_logits_raw[0] != '\0' &&
        std::strcmp(dump_rec_logits_raw, "0") != 0 &&
        std::strcmp(dump_rec_logits_raw, "false") != 0 &&
        std::strcmp(dump_rec_logits_raw, "FALSE") != 0;
    const bool need_rec_debug_meta = debug_trace || dump_rec_logits;

    std::string load_err;
    const auto load_started = std::chrono::steady_clock::now();
    Image img;
    if (!load_image_file(image_path, img, &load_err)) {
        set_error(err, load_err);
        return {};
    }
    if (profile_stages) {
        load_ms += elapsed_ms_since(load_started);
    }
    if (profile_stages) {
        std::ostringstream os;
        os << image_path.filename().string() << " load_ms=" << std::fixed << std::setprecision(3)
           << load_ms;
        debug_log(os.str());
    }
    return run_pipeline(engine, img, image_path.filename().string(), det_resize_long, score_thresh, debug_trace, err);
}
#endif  // BUZHIDAO_HAVE_PADDLE_INFERENCE

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
bool predictor_flag_enabled(
    const char* global_env_name,
    const char* stage_env_name,
    bool default_value
) {
    bool parsed = default_value;
    if (parse_env_bool(std::getenv(global_env_name), &parsed)) {
        return parsed;
    }
    if (stage_env_name != nullptr && parse_env_bool(std::getenv(stage_env_name), &parsed)) {
        return parsed;
    }
    return default_value;
}

void configure_predictor(
    paddle_infer::Config* config,
    const std::pair<fs::path, fs::path>& model_pair,
    bool use_gpu,
    const char* predictor_kind
) {
    config->SetModel(model_pair.first.string(), model_pair.second.string());
    const std::string kind = predictor_kind != nullptr ? predictor_kind : "";
    const char* mkldnn_stage_env = kind == "det"
        ? "BUZHIDAO_PADDLE_FFI_DET_MKLDNN"
        : kind == "cls"
            ? "BUZHIDAO_PADDLE_FFI_CLS_MKLDNN"
            : kind == "rec"
                ? "BUZHIDAO_PADDLE_FFI_REC_MKLDNN"
                : nullptr;
    const char* new_ir_stage_env = kind == "det"
        ? "BUZHIDAO_PADDLE_FFI_DET_NEW_IR"
        : kind == "cls"
            ? "BUZHIDAO_PADDLE_FFI_CLS_NEW_IR"
            : kind == "rec"
                ? "BUZHIDAO_PADDLE_FFI_REC_NEW_IR"
                : nullptr;
    const char* new_executor_stage_env = kind == "det"
        ? "BUZHIDAO_PADDLE_FFI_DET_NEW_EXECUTOR"
        : kind == "cls"
            ? "BUZHIDAO_PADDLE_FFI_CLS_NEW_EXECUTOR"
            : kind == "rec"
                ? "BUZHIDAO_PADDLE_FFI_REC_NEW_EXECUTOR"
                : nullptr;
    const char* cpu_threads_stage_env = kind == "det"
        ? "BUZHIDAO_PADDLE_FFI_DET_CPU_THREADS"
        : kind == "cls"
            ? "BUZHIDAO_PADDLE_FFI_CLS_CPU_THREADS"
            : kind == "rec"
                ? "BUZHIDAO_PADDLE_FFI_REC_CPU_THREADS"
                : nullptr;
    const char* onednn_ops_stage_env = kind == "det"
        ? "BUZHIDAO_PADDLE_FFI_DET_ONEDNN_OPS"
        : kind == "cls"
            ? "BUZHIDAO_PADDLE_FFI_CLS_ONEDNN_OPS"
            : kind == "rec"
                ? "BUZHIDAO_PADDLE_FFI_REC_ONEDNN_OPS"
                : nullptr;
    const bool default_new_ir = kind == "rec" ? false : true;
    const bool enable_new_ir =
        predictor_flag_enabled("BUZHIDAO_PADDLE_FFI_NEW_IR", new_ir_stage_env, default_new_ir);
    const bool enable_new_executor =
        predictor_flag_enabled("BUZHIDAO_PADDLE_FFI_NEW_EXECUTOR", new_executor_stage_env, true);
    if (use_gpu) {
        config->EnableUseGpu(256, 0);
        config->EnableNewIR(enable_new_ir);
        config->EnableNewExecutor(enable_new_executor);
        config->SetOptimizationLevel(3);
    } else {
        int cpu_threads = kind == "rec" ? 4 : 10;
        int env_cpu_threads = 0;
        if (parse_env_int(std::getenv("BUZHIDAO_PADDLE_FFI_CPU_THREADS"), &env_cpu_threads) &&
            env_cpu_threads > 0) {
            cpu_threads = env_cpu_threads;
        }
        if (cpu_threads_stage_env != nullptr &&
            parse_env_int(std::getenv(cpu_threads_stage_env), &env_cpu_threads) &&
            env_cpu_threads > 0) {
            cpu_threads = env_cpu_threads;
        }
        const unsigned int hw_threads = std::thread::hardware_concurrency();
        const int max_threads = hw_threads > 0
            ? static_cast<int>(std::min<unsigned int>(hw_threads, 64u))
            : 64;
        cpu_threads = std::clamp(cpu_threads, 1, std::max(1, max_threads));
        config->DisableGpu();
        const bool enable_mkldnn =
            predictor_flag_enabled("BUZHIDAO_PADDLE_FFI_MKLDNN", mkldnn_stage_env, true);
        if (enable_mkldnn) {
            config->EnableONEDNN();
            config->SetOnednnCacheCapacity(10);
            std::unordered_set<std::string> enabled_ops = parse_env_csv_set(
                std::getenv("BUZHIDAO_PADDLE_FFI_ONEDNN_OPS")
            );
            if (onednn_ops_stage_env != nullptr) {
                const auto stage_ops = parse_env_csv_set(std::getenv(onednn_ops_stage_env));
                if (!stage_ops.empty()) {
                    enabled_ops = stage_ops;
                }
            }
            if (!enabled_ops.empty()) {
                config->SetONEDNNOp(enabled_ops);
            }
        }
        config->SetCpuMathLibraryNumThreads(cpu_threads);
        config->EnableNewIR(enable_new_ir);
        config->EnableNewExecutor(enable_new_executor);
        config->SetOptimizationLevel(3);
    }
    config->EnableMemoryOptim(true);
#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
    if (debug_enabled() || std::getenv("BUZHIDAO_PADDLE_FFI_DUMP_PREDICTOR_CONFIG") != nullptr) {
        std::ostringstream os;
        os << "predictor_config kind=" << kind
           << ", summary=" << config->Summary();
        if (auto* pass_builder = config->pass_builder()) {
            os << ", passes=" << pass_builder->DebugString();
        }
        debug_log(os.str());
    }
#endif
}

bool warmup_det_predictor(buzhi_ocr_engine* engine, std::string* err) {
    Image det_image = make_warmup_pattern_image(320, 320);
    std::string det_err;
    run_det(
        engine->det_predictor,
        det_image,
        0,
        engine->det_cfg,
        engine->det_cfg.det_norm,
        engine->det_options,
        &det_err
    );
    if (!det_err.empty()) {
        set_error_if_empty(err, det_err);
        return false;
    }
    return true;
}

bool warmup_cls_predictor(buzhi_ocr_engine* engine, std::string* err) {
    std::vector<Image> cls_inputs;
    cls_inputs.reserve(6);
    std::vector<const Image*> cls_ptrs;
    cls_ptrs.reserve(6);
    for (int i = 0; i < 6; ++i) {
        cls_inputs.push_back(make_warmup_pattern_image(96 + i * 16, 48 + (i % 2) * 8));
    }
    for (const auto& image : cls_inputs) {
        cls_ptrs.push_back(&image);
    }
    std::string cls_err;
    const auto cls_results = run_cls_batch(
        engine->cls_predictor,
        cls_ptrs,
        engine->cls_cfg,
        &cls_err);
    if (!cls_err.empty()) {
        set_error_if_empty(err, cls_err);
        return false;
    }
    if (cls_results.size() != cls_ptrs.size()) {
        set_error_if_empty(err, "cls warmup 결과 수가 입력 수와 다릅니다");
        return false;
    }
    return true;
}

bool warmup_rec_predictor(buzhi_ocr_engine* engine, std::string* err) {
    std::vector<Image> rec_inputs;
    rec_inputs.reserve(6);
    std::vector<const Image*> rec_ptrs;
    rec_ptrs.reserve(6);
    constexpr std::array<int, 6> kWarmupWidths{{80, 128, 192, 256, 320, 384}};
    for (int width : kWarmupWidths) {
        rec_inputs.push_back(make_warmup_pattern_image(width, engine->rec_cfg.rec_target_h));
    }
    for (const auto& image : rec_inputs) {
        rec_ptrs.push_back(&image);
    }
    std::string rec_err;
    const auto rec_results = run_rec_batch(
        engine->rec_predictor,
        rec_ptrs,
        engine->rec_dict,
        engine->rec_cfg,
        nullptr,
        nullptr,
        &rec_err
    );
    if (!rec_err.empty()) {
        set_error_if_empty(err, rec_err);
        return false;
    }
    if (rec_results.size() != rec_ptrs.size()) {
        set_error_if_empty(err, "rec warmup 결과 수가 입력 수와 다릅니다");
        return false;
    }
    return true;
}
#endif

extern "C" buzhi_ocr_engine_t* buzhi_ocr_create(const char* model_dir, int use_gpu, const char* source, char** err) {
    const char* resolved_model_dir = (model_dir != nullptr && model_dir[0] != '\0')
        ? model_dir
        : std::getenv("BUZHIDAO_PADDLE_FFI_MODEL_DIR");
    if (resolved_model_dir == nullptr || resolved_model_dir[0] == '\0') {
        set_error(err, "model_dir is empty");
        return nullptr;
    }
    const char* resolved_source = (source != nullptr && source[0] != '\0')
        ? source
        : std::getenv("BUZHIDAO_PADDLE_FFI_SOURCE");

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
    const std::string preferred_model_hint = resolve_model_preference();
    const std::string preferred_lang = resolve_preferred_lang();
    const std::string explicit_lang = (resolved_source != nullptr && resolved_source[0] != '\0')
        ? normalize_hint(resolved_source)
        : std::string{};
    const std::string selected_lang = [&]() {
        if (explicit_lang.empty()) {
            return preferred_lang;
        }
        if (
            explicit_lang == "ch_tra" ||
            explicit_lang == "chinese_cht" ||
            explicit_lang == "zh-tw" ||
            explicit_lang == "zh_tw" ||
            explicit_lang == "zh-hant" ||
            explicit_lang == "zh_hant"
        ) {
            return std::string("chinese_cht");
        }
        if (
            explicit_lang == "cn" ||
            explicit_lang == "zh" ||
            explicit_lang == "ch" ||
            explicit_lang == "chi" ||
            explicit_lang == "chinese" ||
            explicit_lang.rfind("zh-", 0) == 0 ||
            explicit_lang.rfind("zh_", 0) == 0
        ) {
            return std::string("ch");
        }
        if (explicit_lang == "en" || explicit_lang == "eng" || explicit_lang == "english") {
            return std::string("en");
        }
        return explicit_lang;
    }();
    const fs::path model_root(resolved_model_dir);
    const auto det_model = resolve_model_pair(
        model_root,
        "det",
        preferred_model_hint,
        selected_lang,
        {}
    );
    debug_log(std::string("model pair det resolved: ") + det_model.first.string() + " / " + det_model.second.string());
    const std::string det_family = [&]() {
        if (det_model.first.empty()) {
            return std::string{};
        }
        return infer_model_family_hint(det_model.first.parent_path(), "det");
    }();
    const auto cls_model = resolve_model_pair(
        model_root,
        "cls",
        preferred_model_hint,
        selected_lang,
        det_family
    );
    debug_log(std::string("model pair cls resolved: ") + cls_model.first.string() + " / " + cls_model.second.string());
    const auto rec_model = resolve_model_pair(
        model_root,
        "rec",
        preferred_model_hint,
        selected_lang,
        det_family
    );
    debug_log(std::string("model pair rec resolved: ") + rec_model.first.string() + " / " + rec_model.second.string());
    if (det_model.first.empty() || cls_model.first.empty() || rec_model.first.empty()) {
        set_error(
            err,
            "Paddle 모델 파일을 찾을 수 없습니다. 모델 루트 디렉터리 아래 det/cls/rec 추론 모델이 있어야 합니다."
        );
        return nullptr;
    }
    debug_log(std::string("selected det=") + det_model.first.string() + ", cls=" + cls_model.first.string() +
              ", rec=" + rec_model.first.string());

    auto* engine = new buzhi_ocr_engine();
    engine->use_gpu = use_gpu;
    engine->model_dir = resolved_model_dir;
    engine->rec_model_dir = rec_model.first.parent_path();
    engine->det_cfg = load_model_preprocess_cfg(det_model.first.parent_path());
    engine->cls_cfg = load_model_preprocess_cfg(cls_model.first.parent_path());
    engine->rec_cfg = load_model_preprocess_cfg(rec_model.first.parent_path());
    int env_rec_max_w = 0;
    if (parse_env_int(std::getenv("BUZHIDAO_PADDLE_FFI_REC_MAX_W"), &env_rec_max_w) &&
        env_rec_max_w > 0) {
        engine->rec_cfg.rec_max_w = std::max(engine->rec_cfg.rec_target_w, env_rec_max_w);
    }
    if (!engine->rec_cfg.rec_dict.empty()) {
        engine->rec_dict = engine->rec_cfg.rec_dict;
    } else {
        std::string dict_error;
        engine->rec_dict = load_recognition_dict(engine->rec_model_dir, &dict_error);
        if (engine->rec_dict.empty()) {
            delete engine;
            set_error(err, std::string("rec_dict를 찾지 못했습니다: ") + dict_error);
            return nullptr;
        }
    }
    {
        std::string dict_error;
        if (!validate_recognition_dict(&engine->rec_dict, &dict_error)) {
            delete engine;
            set_error(err, std::string("rec_dict가 유효하지 않습니다: ") + dict_error);
            return nullptr;
        }
    }
    engine->det_options = resolve_det_options(engine->det_cfg);
    try {
        configure_predictor(&engine->det_config, det_model, use_gpu != 0, "det");
        configure_predictor(&engine->cls_config, cls_model, use_gpu != 0, "cls");
        configure_predictor(&engine->rec_config, rec_model, use_gpu != 0, "rec");
        engine->det_predictor = paddle_infer::CreatePredictor(engine->det_config);
        engine->cls_predictor = paddle_infer::CreatePredictor(engine->cls_config);
        engine->rec_predictor = paddle_infer::CreatePredictor(engine->rec_config);
    } catch (const std::exception& ex) {
        delete engine;
        set_error(err, std::string("Paddle predictor 생성 실패: ") + ex.what());
        return nullptr;
    }
#else
    auto* engine = new buzhi_ocr_engine();
    engine->use_gpu = use_gpu;
    engine->model_dir = resolved_model_dir;
#endif
    return engine;
}

extern "C" void buzhi_ocr_destroy(buzhi_ocr_engine_t* engine) {
    delete engine;
}

extern "C" int buzhi_ocr_warmup_predictors(
    buzhi_ocr_engine_t* engine,
    char** err
) {
    if (engine == nullptr) {
        set_error(err, "engine is null");
        return 0;
    }

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
    std::string warmup_err;
    if (!warmup_det_predictor(engine, &warmup_err) ||
        !warmup_cls_predictor(engine, &warmup_err) ||
        !warmup_rec_predictor(engine, &warmup_err)) {
        set_error(err, warmup_err);
        return 0;
    }
    return 1;
#else
    set_error(
        err,
        "Paddle FFI bridge skeleton is compiled, but Paddle Inference SDK is not linked. Download and configure paddle_inference package under .paddle_inference."
    );
    return 0;
#endif
}

extern "C" char* buzhi_ocr_run_image_file(
    buzhi_ocr_engine_t* engine,
    const char* image_path,
    int det_resize_long,
    float score_thresh,
    int debug_trace,
    char** err
) {
    if (engine == nullptr) {
        set_error(err, "engine is null");
        return nullptr;
    }
    if (image_path == nullptr || image_path[0] == '\0') {
        set_error(err, "image_path is empty");
        return nullptr;
    }
    if (score_thresh < 0.0f) {
        score_thresh = 0.0f;
    } else if (score_thresh > 1.0f) {
        score_thresh = 1.0f;
    }

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
    PipelineOutput result = run_pipeline_from_path(
        engine,
        fs::path(image_path),
        det_resize_long,
        score_thresh,
        debug_trace,
        err
    );
    std::string json = serialize_pipeline_output_json(result, debug_trace != 0);
    free_pipeline_output(&result);
    return dup_string(json);
#else
    set_error(
        err,
        "Paddle FFI bridge skeleton is compiled, but Paddle Inference SDK is not linked. Download and configure paddle_inference package under .paddle_inference."
    );
    return nullptr;
#endif
}

extern "C" buzhi_ocr_result_t* buzhi_ocr_run_image_file_result(
    buzhi_ocr_engine_t* engine,
    const char* image_path,
    int det_resize_long,
    float score_thresh,
    int debug_trace,
    char** err
) {
    if (engine == nullptr) {
        set_error(err, "engine is null");
        return nullptr;
    }
    if (image_path == nullptr || image_path[0] == '\0') {
        set_error(err, "image_path is empty");
        return nullptr;
    }
    if (score_thresh < 0.0f) {
        score_thresh = 0.0f;
    } else if (score_thresh > 1.0f) {
        score_thresh = 1.0f;
    }

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
    PipelineOutput result = run_pipeline_from_path(
        engine,
        fs::path(image_path),
        det_resize_long,
        score_thresh,
        debug_trace,
        err
    );
    return build_native_result(std::move(result), debug_trace != 0);
#else
    set_error(
        err,
        "Paddle FFI bridge skeleton is compiled, but Paddle Inference SDK is not linked. Download and configure paddle_inference package under .paddle_inference."
    );
    return nullptr;
#endif
}

extern "C" buzhi_ocr_result_t* buzhi_ocr_run_image_rgba_result(
    buzhi_ocr_engine_t* engine,
    const unsigned char* rgba,
    int width,
    int height,
    int stride,
    int det_resize_long,
    float score_thresh,
    int debug_trace,
    char** err
) {
    if (engine == nullptr) {
        set_error(err, "engine is null");
        return nullptr;
    }
    if (rgba == nullptr) {
        set_error(err, "rgba is null");
        return nullptr;
    }
    if (width <= 0 || height <= 0) {
        set_error(err, "image size is invalid");
        return nullptr;
    }
    if (stride < width * 4) {
        set_error(err, "image stride is invalid");
        return nullptr;
    }
    if (score_thresh < 0.0f) {
        score_thresh = 0.0f;
    } else if (score_thresh > 1.0f) {
        score_thresh = 1.0f;
    }

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
    Image img{
        width,
        height,
        4,
        std::vector<uint8_t>(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u),
        PixelLayout::RGBA
    };
    for (int y = 0; y < height; ++y) {
        const auto* src = rgba + static_cast<size_t>(y) * static_cast<size_t>(stride);
        auto* dst = img.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u;
        std::memcpy(dst, src, static_cast<size_t>(width) * 4u);
    }

    PipelineOutput result = run_pipeline(
        engine,
        img,
        "rgba_memory",
        det_resize_long,
        score_thresh,
        debug_trace,
        err
    );
    return build_native_result(std::move(result), debug_trace != 0);
#else
    set_error(
        err,
        "Paddle FFI bridge skeleton is compiled, but Paddle Inference SDK is not linked. Download and configure paddle_inference package under .paddle_inference."
    );
    return nullptr;
#endif
}

extern "C" void buzhi_ocr_free_string(char* s) {
    delete[] s;
}

extern "C" void buzhi_ocr_free_result(buzhi_ocr_result_t* result) {
    if (result == nullptr) {
        return;
    }
    if (result->detections != nullptr) {
        for (int i = 0; i < result->detection_count; ++i) {
            delete[] result->detections[i].text;
        }
        delete[] result->detections;
    }
    if (result->debug_detections != nullptr) {
        for (int i = 0; i < result->debug_detection_count; ++i) {
            delete[] result->debug_detections[i].text;
        }
        delete[] result->debug_detections;
    }
    delete result;
}
