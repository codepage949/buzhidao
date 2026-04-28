#include "bridge_crop.h"

#include "bridge_debug_dump.h"
#include "bridge_geometry.h"
#include "bridge_image.h"
#include "bridge_resample.h"
#include "bridge_rotate.h"
#include "bridge_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#if defined(BUZHIDAO_HAVE_OPENCV)
Image crop_to_bbox(const cv::Mat& img_bgr, const std::array<FloatPoint, 4>& pts, std::string* err) {
    if (img_bgr.empty()) {
        if (err != nullptr) {
            *err = "crop source image가 비어 있습니다";
        }
        return {0, 0, 0, {}};
    }
    std::array<cv::Point2f, 4> crop_points{};
    for (size_t i = 0; i < pts.size(); ++i) {
        crop_points[i] = cv::Point2f(
            static_cast<float>(static_cast<int>(pts[i].x)),
            static_cast<float>(static_cast<int>(pts[i].y))
        );
    }
    const cv::Mat crop_points_view(
        static_cast<int>(crop_points.size()),
        1,
        CV_32FC2,
        crop_points.data()
    );
    const cv::RotatedRect crop_box = cv::minAreaRect(crop_points_view);
    cv::Point2f box_points[4];
    crop_box.points(box_points);
    const std::array<FloatPoint, 4> crop_corners{{
        {box_points[0].x, box_points[0].y},
        {box_points[1].x, box_points[1].y},
        {box_points[2].x, box_points[2].y},
        {box_points[3].x, box_points[3].y},
    }};
    const auto quad = order_crop_box_for_perspective_crop(crop_corners);
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
    const auto quad = order_crop_box_for_perspective_crop(crop_box.corners);
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
