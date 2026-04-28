#include "bridge_config.h"
#include "bridge_env.h"

#include "bridge_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

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
    if (parse_env_float(std::getenv(buzhidao_env::kFfiDetThresh), &env_float)) {
        opts.threshold = clamp01(env_float);
    }
    if (parse_env_float(std::getenv(buzhidao_env::kFfiDetBoxThresh), &env_float)) {
        opts.box_threshold = clamp01(env_float);
    }
    if (parse_env_float(std::getenv(buzhidao_env::kFfiDetMinSide), &env_float)) {
        opts.min_side = clamp_min(env_float, 0.0f);
    }
    if (parse_env_float(std::getenv(buzhidao_env::kFfiDetUnclip), &env_float)) {
        opts.unclip_ratio = clamp_min(env_float, 0.0f);
    }
    int env_candidates = 0;
    if (parse_env_int(std::getenv(buzhidao_env::kFfiDetMaxCandidates), &env_candidates) &&
        env_candidates > 0) {
        opts.max_candidates = env_candidates;
    }
    return opts;
}
