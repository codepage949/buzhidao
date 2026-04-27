#include "bridge_rec.h"

#include "bridge_fs.h"
#include "bridge_image.h"
#include "bridge_predictor.h"
#include "bridge_rec_decode.h"
#include "bridge_tensor.h"
#include "bridge_utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
namespace {

void set_rec_error_if_empty(std::string* err, const std::string& message) {
    if (err != nullptr && err->empty()) {
        *err = message;
    }
}

}  // namespace

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
        set_rec_error_if_empty(err, "rec predictor 실행 실패");
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
        set_rec_error_if_empty(err, "rec batch predictor 실행 실패");
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

#endif
