#include "bridge_debug_dump.h"

#include "bridge_image.h"
#include "bridge_utils.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

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
