#include "bridge_predictor_config.h"

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
        ? "BUZHIDAO_PADDLE_FFI_DET_MKLDNN"
        : kind == "cls"
            ? "BUZHIDAO_PADDLE_FFI_CLS_MKLDNN"
            : kind == "rec"
                ? "BUZHIDAO_PADDLE_FFI_REC_MKLDNN"
                : nullptr;
    const char* new_ir_stage_env = kind == "det"
        ? "BUZHIDAO_PADDLE_FFI_DET_NEW_IR"
        : kind == "cls"
            ? "BUZHIDAO_PADDLE_FFI_CLS_NEW_IR"
            : kind == "rec"
                ? "BUZHIDAO_PADDLE_FFI_REC_NEW_IR"
                : nullptr;
    const char* new_executor_stage_env = kind == "det"
        ? "BUZHIDAO_PADDLE_FFI_DET_NEW_EXECUTOR"
        : kind == "cls"
            ? "BUZHIDAO_PADDLE_FFI_CLS_NEW_EXECUTOR"
            : kind == "rec"
                ? "BUZHIDAO_PADDLE_FFI_REC_NEW_EXECUTOR"
                : nullptr;
    const char* cpu_threads_stage_env = kind == "det"
        ? "BUZHIDAO_PADDLE_FFI_DET_CPU_THREADS"
        : kind == "cls"
            ? "BUZHIDAO_PADDLE_FFI_CLS_CPU_THREADS"
            : kind == "rec"
                ? "BUZHIDAO_PADDLE_FFI_REC_CPU_THREADS"
                : nullptr;
    const char* onednn_ops_stage_env = kind == "det"
        ? "BUZHIDAO_PADDLE_FFI_DET_ONEDNN_OPS"
        : kind == "cls"
            ? "BUZHIDAO_PADDLE_FFI_CLS_ONEDNN_OPS"
            : kind == "rec"
                ? "BUZHIDAO_PADDLE_FFI_REC_ONEDNN_OPS"
                : nullptr;
    const bool default_new_ir = kind == "rec" ? false : true;
    const bool enable_new_ir =
        predictor_flag_enabled("BUZHIDAO_PADDLE_FFI_NEW_IR", new_ir_stage_env, default_new_ir);
    const bool enable_new_executor =
        predictor_flag_enabled("BUZHIDAO_PADDLE_FFI_NEW_EXECUTOR", new_executor_stage_env, true);
    if (use_gpu) {
        config->EnableUseGpu(256, 0);
        config->EnableNewIR(enable_new_ir);
        config->EnableNewExecutor(enable_new_executor);
        config->SetOptimizationLevel(3);
    } else {
        int cpu_threads = kind == "rec" ? 4 : 10;
        int env_cpu_threads = 0;
        if (parse_env_int(std::getenv("BUZHIDAO_PADDLE_FFI_CPU_THREADS"), &env_cpu_threads) &&
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
            predictor_flag_enabled("BUZHIDAO_PADDLE_FFI_MKLDNN", mkldnn_stage_env, true);
        if (enable_mkldnn) {
            config->EnableONEDNN();
            config->SetOnednnCacheCapacity(10);
            std::unordered_set<std::string> enabled_ops = parse_env_csv_set(
                std::getenv("BUZHIDAO_PADDLE_FFI_ONEDNN_OPS")
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
    if (debug_enabled() || std::getenv("BUZHIDAO_PADDLE_FFI_DUMP_PREDICTOR_CONFIG") != nullptr) {
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
