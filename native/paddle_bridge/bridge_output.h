#ifndef BUZHIDAO_PADDLE_BRIDGE_OUTPUT_H
#define BUZHIDAO_PADDLE_BRIDGE_OUTPUT_H

#include "bridge.h"
#include "bridge_types.h"

#include <array>
#include <cstddef>
#include <string>

struct PipelineOutput {
    buzhi_ocr_detection_t* detections{nullptr};
    int detection_count{0};
    int detection_capacity{0};
    buzhi_ocr_debug_detection_t* debug_detections{nullptr};
    int debug_detection_count{0};
    int debug_detection_capacity{0};
};

char* dup_string(const std::string& value);

void reserve_pipeline_output(
    PipelineOutput* output,
    size_t detection_capacity,
    bool include_debug,
    size_t debug_capacity
);
bool append_detection(PipelineOutput* output, const std::array<FloatPoint, 4>& polygon, const std::string& text);
bool append_debug_detection(
    PipelineOutput* output,
    const std::array<FloatPoint, 4>& polygon,
    const std::string& text,
    float score,
    bool accepted
);
void reset_pipeline_output(PipelineOutput* output);
void free_pipeline_output(PipelineOutput* output);
std::string serialize_pipeline_output_json(const PipelineOutput& output, bool include_debug);
buzhi_ocr_result_t* build_native_result(PipelineOutput&& output, bool include_debug);

#endif
