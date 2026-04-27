#ifndef BUZHIDAO_PADDLE_BRIDGE_PIPELINE_H
#define BUZHIDAO_PADDLE_BRIDGE_PIPELINE_H

#include "bridge_output.h"
#include "bridge_types.h"

#include <filesystem>
#include <string>

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)

struct buzhi_ocr_engine;

PipelineOutput run_pipeline(
    buzhi_ocr_engine* engine,
    const Image& img,
    const std::string& image_label,
    int det_resize_long,
    float score_thresh,
    int debug_trace,
    char** err
);

PipelineOutput run_pipeline_from_path(
    buzhi_ocr_engine* engine,
    const std::filesystem::path& image_path,
    int det_resize_long,
    float score_thresh,
    int debug_trace,
    char** err
);

#endif

#endif
