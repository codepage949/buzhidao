#ifndef BUZHIDAO_PADDLE_BRIDGE_ENV_H
#define BUZHIDAO_PADDLE_BRIDGE_ENV_H

namespace buzhidao_env {

constexpr const char* kFfiSource = "BUZHIDAO_PADDLE_FFI_SOURCE";
constexpr const char* kFfiModelDir = "BUZHIDAO_PADDLE_FFI_MODEL_DIR";
constexpr const char* kFfiModelHint = "BUZHIDAO_PADDLE_FFI_MODEL_HINT";
constexpr const char* kFfiTrace = "BUZHIDAO_PADDLE_FFI_TRACE";
constexpr const char* kFfiProfileStages = "BUZHIDAO_PADDLE_FFI_PROFILE_STAGES";
constexpr const char* kFfiDumpDir = "BUZHIDAO_PADDLE_FFI_DUMP_DIR";

constexpr const char* kFfiDetResizeLong = "BUZHIDAO_PADDLE_FFI_DET_RESIZE_LONG";
constexpr const char* kFfiDetThresh = "BUZHIDAO_PADDLE_FFI_DET_THRESH";
constexpr const char* kFfiDetBoxThresh = "BUZHIDAO_PADDLE_FFI_DET_BOX_THRESH";
constexpr const char* kFfiDetMinSide = "BUZHIDAO_PADDLE_FFI_DET_MIN_SIDE";
constexpr const char* kFfiDetUnclip = "BUZHIDAO_PADDLE_FFI_DET_UNCLIP";
constexpr const char* kFfiDetMaxCandidates = "BUZHIDAO_PADDLE_FFI_DET_MAX_CANDIDATES";
constexpr const char* kFfiDumpDet = "BUZHIDAO_PADDLE_FFI_DUMP_DET";

constexpr const char* kFfiRecMaxW = "BUZHIDAO_PADDLE_FFI_REC_MAX_W";
constexpr const char* kFfiRecBatchWidthBudget = "BUZHIDAO_PADDLE_FFI_REC_BATCH_WIDTH_BUDGET";
constexpr const char* kFfiDumpRecLogits = "BUZHIDAO_PADDLE_FFI_DUMP_REC_LOGITS";
constexpr const char* kFfiDebugCtcAll = "BUZHIDAO_PADDLE_FFI_DEBUG_CTC_ALL";

constexpr const char* kFfiCpuThreads = "BUZHIDAO_PADDLE_FFI_CPU_THREADS";
constexpr const char* kFfiMkldnn = "BUZHIDAO_PADDLE_FFI_MKLDNN";
constexpr const char* kFfiNewIr = "BUZHIDAO_PADDLE_FFI_NEW_IR";
constexpr const char* kFfiNewExecutor = "BUZHIDAO_PADDLE_FFI_NEW_EXECUTOR";
constexpr const char* kFfiOnednnOps = "BUZHIDAO_PADDLE_FFI_ONEDNN_OPS";
constexpr const char* kFfiDumpPredictorConfig = "BUZHIDAO_PADDLE_FFI_DUMP_PREDICTOR_CONFIG";

constexpr const char* kFfiDetMkldnn = "BUZHIDAO_PADDLE_FFI_DET_MKLDNN";
constexpr const char* kFfiClsMkldnn = "BUZHIDAO_PADDLE_FFI_CLS_MKLDNN";
constexpr const char* kFfiRecMkldnn = "BUZHIDAO_PADDLE_FFI_REC_MKLDNN";
constexpr const char* kFfiDetNewIr = "BUZHIDAO_PADDLE_FFI_DET_NEW_IR";
constexpr const char* kFfiClsNewIr = "BUZHIDAO_PADDLE_FFI_CLS_NEW_IR";
constexpr const char* kFfiRecNewIr = "BUZHIDAO_PADDLE_FFI_REC_NEW_IR";
constexpr const char* kFfiDetNewExecutor = "BUZHIDAO_PADDLE_FFI_DET_NEW_EXECUTOR";
constexpr const char* kFfiClsNewExecutor = "BUZHIDAO_PADDLE_FFI_CLS_NEW_EXECUTOR";
constexpr const char* kFfiRecNewExecutor = "BUZHIDAO_PADDLE_FFI_REC_NEW_EXECUTOR";
constexpr const char* kFfiDetCpuThreads = "BUZHIDAO_PADDLE_FFI_DET_CPU_THREADS";
constexpr const char* kFfiClsCpuThreads = "BUZHIDAO_PADDLE_FFI_CLS_CPU_THREADS";
constexpr const char* kFfiRecCpuThreads = "BUZHIDAO_PADDLE_FFI_REC_CPU_THREADS";
constexpr const char* kFfiDetOnednnOps = "BUZHIDAO_PADDLE_FFI_DET_ONEDNN_OPS";
constexpr const char* kFfiClsOnednnOps = "BUZHIDAO_PADDLE_FFI_CLS_ONEDNN_OPS";
constexpr const char* kFfiRecOnednnOps = "BUZHIDAO_PADDLE_FFI_REC_ONEDNN_OPS";

}  // namespace buzhidao_env

#endif
