#include "bridge.h"
#include "bridge_env.h"

#include "bridge_config.h"
#include "bridge_dict.h"
#include "bridge_engine.h"
#include "bridge_model.h"
#include "bridge_output.h"
#include "bridge_predictor_config.h"
#include "bridge_utils.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
#include <paddle_inference_api.h>
#endif

namespace {

void set_create_error(char** err, const std::string& message) {
    if (err != nullptr) {
        *err = dup_string(message);
    }
}

}  // namespace

extern "C" buzhi_ocr_engine_t* buzhi_ocr_create(const char* model_dir, int use_gpu, const char* source, char** err) {
    const char* resolved_model_dir = (model_dir != nullptr && model_dir[0] != '\0')
        ? model_dir
        : std::getenv(buzhidao_env::kFfiModelDir);
    if (resolved_model_dir == nullptr || resolved_model_dir[0] == '\0') {
        set_create_error(err, "model_dir is empty");
        return nullptr;
    }
    const char* resolved_source = (source != nullptr && source[0] != '\0')
        ? source
        : std::getenv(buzhidao_env::kFfiSource);

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
        set_create_error(
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
    if (parse_env_int(std::getenv(buzhidao_env::kFfiRecMaxW), &env_rec_max_w) &&
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
            set_create_error(err, std::string("rec_dict를 찾지 못했습니다: ") + dict_error);
            return nullptr;
        }
    }
    {
        std::string dict_error;
        if (!validate_recognition_dict(&engine->rec_dict, &dict_error)) {
            delete engine;
            set_create_error(err, std::string("rec_dict가 유효하지 않습니다: ") + dict_error);
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
        set_create_error(err, std::string("Paddle predictor 생성 실패: ") + ex.what());
        return nullptr;
    }
#else
    auto* engine = new buzhi_ocr_engine();
    engine->use_gpu = use_gpu;
    engine->model_dir = resolved_model_dir;
#endif
    return engine;
}
