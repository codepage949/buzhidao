#include "bridge_det_utils.h"

#include "bridge_debug_format.h"
#include "bridge_geometry.h"
#include "bridge_utils.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <queue>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

#if __has_include(<opencv2/opencv.hpp>)
#include <opencv2/opencv.hpp>
#define BUZHIDAO_HAVE_OPENCV 1
#endif

float round_half_to_even(float value) {
    const double floor_value = std::floor(value);
    const double fraction = value - floor_value;
    constexpr double kEpsilon = 1e-6;
    if (std::fabs(fraction - 0.5) <= kEpsilon) {
        const auto floor_int = static_cast<long long>(floor_value);
        return static_cast<float>((floor_int % 2 == 0) ? floor_value : floor_value + 1.0);
    }
    return static_cast<float>(std::floor(value + 0.5));
}

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
                round_half_to_even(unclipped_mini.first[i].x * x_scale),
                0.0f,
                static_cast<float>(src_w)
            );
            scaled[i].y = std::clamp(
                round_half_to_even(unclipped_mini.first[i].y * y_scale),
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
                    round_half_to_even(unclipped_mini.first[i].x * x_scale),
                    0.0f,
                    static_cast<float>(src_w)
                );
                pts[i].y = std::clamp(
                    round_half_to_even(unclipped_mini.first[i].y * y_scale),
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


float score_box(
    const std::vector<float>& pred,
    int pred_h,
    int pred_w,
    const std::array<FloatPoint, 4>& box,
    ScoreBoxDebug* debug
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
