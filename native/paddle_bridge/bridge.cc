#include "bridge.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>

#if __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#endif

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
#include <paddle_inference_api.h>
#endif

struct buzhi_ocr_engine {
	int use_gpu;
	std::string model_dir;
#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
	paddle_infer::Config det_config;
	paddle_infer::Config cls_config;
	paddle_infer::Config rec_config;
	std::shared_ptr<paddle_infer::Predictor> det_predictor;
	std::shared_ptr<paddle_infer::Predictor> cls_predictor;
	std::shared_ptr<paddle_infer::Predictor> rec_predictor;
#endif
};

namespace {
char* dup_string(const std::string& value) {
	char* out = new char[value.size() + 1];
	std::memcpy(out, value.c_str(), value.size() + 1);
	return out;
}

void set_error(char** err, const std::string& message) {
	if (err != nullptr) {
		*err = dup_string(message);
	}
}

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE) && __has_include(<filesystem>)
bool file_exists(const fs::path& path) {
	return fs::exists(path) && fs::is_regular_file(path);
}

std::pair<std::string, std::string> resolve_model_pair(
	const std::string& root,
	const std::string& stem
) {
	const fs::path base(root);
	const fs::path subdir = base / stem;

	const fs::path direct_json = base / (stem + ".json");
	const fs::path direct_params = base / (stem + ".pdiparams");
	if (file_exists(direct_json) && file_exists(direct_params)) {
		return {direct_json.string(), direct_params.string()};
	}

	const fs::path infer_json = subdir / "inference.json";
	const fs::path infer_params = subdir / "inference.pdiparams";
	if (file_exists(infer_json) && file_exists(infer_params)) {
		return {infer_json.string(), infer_params.string()};
	}

	const fs::path infer_pdmodel = subdir / "inference.pdmodel";
	if (file_exists(infer_pdmodel) && file_exists(infer_params)) {
		return {infer_pdmodel.string(), infer_params.string()};
	}

	return {"", ""};
}

void configure_predictor(
	paddle_infer::Config* config,
	const std::pair<std::string, std::string>& model_pair,
	bool use_gpu
) {
	config->SetModel(model_pair.first, model_pair.second);
	config->SwitchIrOptim(false);
	config->SwitchUseFeedFetchOps(false);
	if (use_gpu) {
		config->EnableUseGpu(256, 0);
	} else {
		config->DisableGpu();
		config->SetCpuMathLibraryNumThreads(1);
	}
}
#endif
} // namespace

extern "C" buzhi_ocr_engine_t* buzhi_ocr_create(const char* model_dir, int use_gpu, char** err) {
	if (model_dir == nullptr || model_dir[0] == '\0') {
		set_error(err, "model_dir is empty");
		return nullptr;
	}

	auto* engine = new buzhi_ocr_engine();
	engine->use_gpu = use_gpu;
	engine->model_dir = model_dir;
#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
	const auto det_model = resolve_model_pair(engine->model_dir, "det");
	const auto cls_model = resolve_model_pair(engine->model_dir, "cls");
	const auto rec_model = resolve_model_pair(engine->model_dir, "rec");
	if (det_model.first.empty() || cls_model.first.empty() || rec_model.first.empty()) {
		delete engine;
		set_error(
			err,
			"Paddle 모델 파일을 찾을 수 없습니다. PADDLE_MODEL_DIR 아래에 det/cls/rec 추론 모델이 있어야 합니다."
		);
		return nullptr;
	}

	try {
		configure_predictor(&engine->det_config, det_model, use_gpu != 0);
		configure_predictor(&engine->cls_config, cls_model, use_gpu != 0);
		configure_predictor(&engine->rec_config, rec_model, use_gpu != 0);
		engine->det_predictor = paddle_infer::CreatePredictor(engine->det_config);
		engine->cls_predictor = paddle_infer::CreatePredictor(engine->cls_config);
		engine->rec_predictor = paddle_infer::CreatePredictor(engine->rec_config);
	} catch (const std::exception& ex) {
		delete engine;
		set_error(err, std::string("Paddle predictor 생성 실패: ") + ex.what());
		return nullptr;
	}
#endif
	return engine;
}

extern "C" void buzhi_ocr_destroy(buzhi_ocr_engine_t* engine) {
	delete engine;
}

extern "C" char* buzhi_ocr_run_image_file(
	buzhi_ocr_engine_t* engine,
	const char* image_path,
	int det_resize_long,
	float score_thresh,
	int debug_trace,
	char** err
) {
	if (engine == nullptr) {
		set_error(err, "engine is null");
		return nullptr;
	}
	if (image_path == nullptr || image_path[0] == '\0') {
		set_error(err, "image_path is empty");
		return nullptr;
	}

	(void)det_resize_long;
	(void)score_thresh;
	(void)debug_trace;

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
	set_error(
		err,
		"Paddle Inference SDK detected, but predictor execution is not implemented yet"
	);
	return nullptr;
#else
	set_error(
		err,
		"Paddle FFI bridge skeleton is compiled, but Paddle Inference SDK is not linked. Set PADDLE_INFERENCE_DIR to a downloaded paddle_inference package root."
	);
	return nullptr;
#endif
}

extern "C" void buzhi_ocr_free_string(char* s) {
	delete[] s;
}
