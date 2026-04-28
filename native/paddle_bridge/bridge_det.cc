#include "bridge_det.h"

#include "bridge_debug_dump.h"
#include "bridge_det_utils.h"
#include "bridge_fs.h"
#include "bridge_image.h"
#include "bridge_predictor.h"
#include "bridge_tensor.h"
#include "bridge_utils.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
namespace {

void set_det_error_if_empty(std::string* err, const std::string& message) {
    if (err != nullptr && err->empty()) {
        *err = message;
    }
}

}  // namespace

std::vector<BBox> run_det(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const Image& img,
    int det_resize_long,
    const ModelPreprocessCfg& det_cfg,
    const NormalizeCfg& det_norm,
    const DetOptions& det_options,
    DetScratch* scratch,
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
    DetScratch local_scratch;
    DetScratch& buffers = scratch != nullptr ? *scratch : local_scratch;
    const auto preprocess_started = std::chrono::steady_clock::now();
    float* input = nullptr;
    size_t input_len = 0;
    if (!preprocess_det_into_buffer(
        img,
        det_limit_side_len,
        det_limit_type,
        det_cfg.det_max_side_limit,
        det_norm,
        &buffers.input,
        &resized_h,
        &resized_w,
        &input,
        &input_len,
        err
    )) {
        set_det_error_if_empty(err, "det 입력 텐서를 구성할 수 없습니다");
        return {};
    }
    if (profile_stages) {
        preprocess_ms += elapsed_ms_since(preprocess_started);
    }
    if (input == nullptr || input_len == 0 || resized_h == 0 || resized_w == 0) {
        if (err != nullptr) {
            *err = "det 입력 텐서를 구성할 수 없습니다";
        }
        return {};
    }
    buffers.input_shape.assign({1, 3, resized_h, resized_w});
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
    size_t out_len = 0;
    if (!run_predictor_into_buffer(
            predictor,
            input,
            input_len,
            buffers.input_shape,
            &buffers.output,
            &out_len,
            buffers.output_shape,
            err)) {
        set_det_error_if_empty(err, "det predictor 실행 실패");
        return {};
    }
    if (profile_stages) {
        predictor_ms += elapsed_ms_since(predictor_started);
    }
    const float* out = buffers.output.get();
    const auto& out_shape = buffers.output_shape;
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
            if (out_len < pred_area) {
                if (err != nullptr) {
                    *err = "det 출력 길이가 예측 맵보다 짧습니다";
                }
                return {};
            }
            const auto postprocess_started = std::chrono::steady_clock::now();
            buffers.single_channel.assign(pred_h * pred_w, 0.0f);
            auto& single = buffers.single_channel;
            for (int i = 0; i < pred_h * pred_w; ++i) {
                float best = out[i];
                for (int ch = 1; ch < c; ++ch) {
                    const size_t idx = static_cast<size_t>(ch) * pred_h * pred_w + i;
                    if (idx < out_len) {
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
    if (out_len < pred_area) {
        if (err != nullptr) {
            *err = "det 출력 길이가 예측 맵보다 짧습니다";
        }
        return {};
    }
    ensure_probability_map(buffers.output.get(), out_len);
    log_det_map_stats("run_det pred", buffers.output.get(), out_len, pred_h, pred_w);
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
                for (size_t i = 0; i < input_len; ++i) {
                    if (i > 0) {
                        ofs << ",";
                    }
                    ofs << std::setprecision(9) << input[i];
                }
                ofs << "],\n";
                ofs << "  \"values\": [";
                for (size_t i = 0; i < out_len; ++i) {
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
    auto boxes = db_postprocess(buffers.output.get(), out_len, pred_h, pred_w, img.height, img.width, det_options);
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

#endif
