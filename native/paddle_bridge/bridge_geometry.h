#ifndef BUZHIDAO_PADDLE_BRIDGE_GEOMETRY_H
#define BUZHIDAO_PADDLE_BRIDGE_GEOMETRY_H

#include "bridge_types.h"

#include <array>
#include <tuple>
#include <utility>
#include <vector>

float point_distance(const FloatPoint& a, const FloatPoint& b);
FloatPoint lerp_point(const FloatPoint& a, const FloatPoint& b, float t);
float cross(const FloatPoint& o, const FloatPoint& a, const FloatPoint& b);
std::vector<FloatPoint> convex_hull(std::vector<FloatPoint> points);
std::array<FloatPoint, 4> rect_to_points(const OrientedRect& rect);
std::array<FloatPoint, 4> order_clockwise(const std::array<FloatPoint, 4>& pts);
std::array<FloatPoint, 4> order_crop_box_for_perspective_crop(const std::array<FloatPoint, 4>& pts);
std::tuple<std::array<FloatPoint, 4>, int, int> describe_crop_to_bbox(
    const std::array<FloatPoint, 4>& pts
);
std::pair<std::array<FloatPoint, 4>, float> get_mini_box(const std::array<FloatPoint, 4>& pts);
std::pair<std::array<FloatPoint, 4>, float> get_mini_box(const OrientedRect& rect);
OrientedRect min_area_rect(const std::vector<FloatPoint>& hull);
MinAreaRectBox min_area_rect_box_like_opencv(const std::vector<FloatPoint>& points);
OrientedRect expand_rect(const OrientedRect& rect, float ratio);
bool point_in_quad(const FloatPoint& p, const std::array<FloatPoint, 4>& quad);
float polygon_area(const std::array<FloatPoint, 4>& pts);
std::vector<FloatPoint> unclip(const std::array<FloatPoint, 4>& pts, float ratio);
std::vector<FloatPoint> simplify_contour(const std::vector<FloatPoint>& contour);
std::vector<FloatPoint> dedupe_contour_points(const std::vector<FloatPoint>& contour);
std::vector<FloatPoint> normalize_contour_to_pixel_grid(
    const std::vector<FloatPoint>& contour,
    int max_x,
    int max_y
);
std::vector<FloatPoint> compress_dense_contour_runs(const std::vector<FloatPoint>& contour);

#endif
