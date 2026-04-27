#include "bridge_geometry.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>
#include <tuple>

#if __has_include(<clipper.hpp>)
#include <clipper.hpp>
#define BUZHIDAO_HAVE_PYCLIPPER_CLIPPER 1
#endif

#if __has_include(<opencv2/opencv.hpp>)
#include <opencv2/opencv.hpp>
#define BUZHIDAO_HAVE_OPENCV 1
#endif

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
