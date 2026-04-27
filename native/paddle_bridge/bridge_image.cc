#include "bridge_image.h"

#include "bridge_config.h"
#include "bridge_fs.h"
#include "bridge_resample.h"
#include "bridge_utils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#endif

namespace fs = std::filesystem;

uint8_t image_blue_at(const Image& image, size_t idx) {
    return image.layout == PixelLayout::RGBA ? image.pixels[idx + 2] : image.pixels[idx + 0];
}

uint8_t image_green_at(const Image& image, size_t idx) {
    return image.pixels[idx + 1];
}

uint8_t image_red_at(const Image& image, size_t idx) {
    return image.layout == PixelLayout::RGBA ? image.pixels[idx + 0] : image.pixels[idx + 2];
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


Image load_oriented_image(const fs::path& path, std::string* err) {
    Image img{0, 0, 0, {}};
    if (!load_bmp(path, img, err)) {
        return img;
    }
    return img;
}
