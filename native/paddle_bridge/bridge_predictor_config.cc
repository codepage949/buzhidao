#include "bridge_predictor_config.h"
#include "bridge_env.h"

#include "bridge_utils.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
bool predictor_flag_enabled(
    const char* global_env_name,
    const char* stage_env_name,
    bool default_value
) {
    bool parsed = default_value;
    if (parse_env_bool(std::getenv(global_env_name), &parsed)) {
        return parsed;
    }
    if (stage_env_name != nullptr && parse_env_bool(std::getenv(stage_env_name), &parsed)) {
        return parsed;
    }
    return default_value;
}

void configure_predictor(
    paddle_infer::Config* config,
    const std::pair<std::filesystem::path, std::filesystem::path>& model_pair,
    bool use_gpu,
    const char* predictor_kind
) {
    config->SetModel(model_pair.first.string(), model_pair.second.string());
    const std::string kind = predictor_kind != nullptr ? predictor_kind : "";
    const char* mkldnn_stage_env = kind == "det"
        ? buzhidao_env::kFfiDetMkldnn
        : kind == "cls"
            ? buzhidao_env::kFfiClsMkldnn
            : kind == "rec"
                ? buzhidao_env::kFfiRecMkldnn
                : nullptr;
    const char* new_ir_stage_env = kind == "det"
        ? buzhidao_env::kFfiDetNewIr
        : kind == "cls"
            ? buzhidao_env::kFfiClsNewIr
            : kind == "rec"
                ? buzhidao_env::kFfiRecNewIr
                : nullptr;
    const char* new_executor_stage_env = kind == "det"
        ? buzhidao_env::kFfiDetNewExecutor
        : kind == "cls"
            ? buzhidao_env::kFfiClsNewExecutor
            : kind == "rec"
                ? buzhidao_env::kFfiRecNewExecutor
                : nullptr;
    const char* cpu_threads_stage_env = kind == "det"
        ? buzhidao_env::kFfiDetCpuThreads
        : kind == "cls"
            ? buzhidao_env::kFfiClsCpuThreads
            : kind == "rec"
                ? buzhidao_env::kFfiRecCpuThreads
                : nullptr;
    const char* onednn_ops_stage_env = kind == "det"
        ? buzhidao_env::kFfiDetOnednnOps
        : kind == "cls"
            ? buzhidao_env::kFfiClsOnednnOps
            : kind == "rec"
                ? buzhidao_env::kFfiRecOnednnOps
                : nullptr;
    const bool default_new_ir = kind == "rec" ? false : true;
    const bool enable_new_ir =
        predictor_flag_enabled(buzhidao_env::kFfiNewIr, new_ir_stage_env, default_new_ir);
    const bool enable_new_executor =
        predictor_flag_enabled(buzhidao_env::kFfiNewExecutor, new_executor_stage_env, true);
    if (use_gpu) {
        config->EnableUseGpu(256, 0);
        config->EnableNewIR(enable_new_ir);
        config->EnableNewExecutor(enable_new_executor);
        config->SetOptimizationLevel(3);
    } else {
        int cpu_threads = kind == "rec" ? 4 : 10;
        int env_cpu_threads = 0;
        if (parse_env_int(std::getenv(buzhidao_env::kFfiCpuThreads), &env_cpu_threads) &&
            env_cpu_threads > 0) {
            cpu_threads = env_cpu_threads;
        }
        if (cpu_threads_stage_env != nullptr &&
            parse_env_int(std::getenv(cpu_threads_stage_env), &env_cpu_threads) &&
            env_cpu_threads > 0) {
            cpu_threads = env_cpu_threads;
        }
        const unsigned int hw_threads = std::thread::hardware_concurrency();
        const int max_threads = hw_threads > 0
            ? static_cast<int>(std::min<unsigned int>(hw_threads, 64u))
            : 64;
        cpu_threads = std::clamp(cpu_threads, 1, std::max(1, max_threads));
        config->DisableGpu();
        const bool enable_mkldnn =
            predictor_flag_enabled(buzhidao_env::kFfiMkldnn, mkldnn_stage_env, true);
        if (enable_mkldnn) {
            config->EnableONEDNN();
            config->SetOnednnCacheCapacity(10);
            std::unordered_set<std::string> enabled_ops = parse_env_csv_set(
                std::getenv(buzhidao_env::kFfiOnednnOps)
            );
            if (onednn_ops_stage_env != nullptr) {
                const auto stage_ops = parse_env_csv_set(std::getenv(onednn_ops_stage_env));
                if (!stage_ops.empty()) {
                    enabled_ops = stage_ops;
                }
            }
            if (!enabled_ops.empty()) {
                config->SetONEDNNOp(enabled_ops);
            }
        }
        config->SetCpuMathLibraryNumThreads(cpu_threads);
        config->EnableNewIR(enable_new_ir);
        config->EnableNewExecutor(enable_new_executor);
        config->SetOptimizationLevel(3);
    }
    config->EnableMemoryOptim(true);
    if (debug_enabled() || std::getenv(buzhidao_env::kFfiDumpPredictorConfig) != nullptr) {
        std::ostringstream os;
        os << "predictor_config kind=" << kind
           << ", summary=" << config->Summary();
        if (auto* pass_builder = config->pass_builder()) {
            os << ", passes=" << pass_builder->DebugString();
        }
        debug_log(os.str());
    }
}
#endif
