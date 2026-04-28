#include "bridge_output.h"

#include <cstring>
#include <limits>
#include <new>

char* dup_string(const std::string& value) {
    char* out = new (std::nothrow) char[value.size() + 1];
    if (out == nullptr) {
        return nullptr;
    }
    std::memcpy(out, value.c_str(), value.size() + 1);
    return out;
}

void reserve_pipeline_output(
    PipelineOutput* output,
    size_t detection_capacity,
    bool include_debug,
    size_t debug_capacity
) {
    if (output == nullptr) {
        return;
    }
    if (detection_capacity > static_cast<size_t>(std::numeric_limits<int>::max())) {
        detection_capacity = static_cast<size_t>(std::numeric_limits<int>::max());
    }
    if (debug_capacity > static_cast<size_t>(std::numeric_limits<int>::max())) {
        debug_capacity = static_cast<size_t>(std::numeric_limits<int>::max());
    }
    if (detection_capacity > 0) {
        output->detections = new (std::nothrow) buzhi_ocr_detection_t[detection_capacity]();
        if (output->detections == nullptr) {
            output->detection_capacity = 0;
            return;
        }
        output->detection_capacity = static_cast<int>(detection_capacity);
    }
    if (include_debug && debug_capacity > 0) {
        output->debug_detections = new (std::nothrow) buzhi_ocr_debug_detection_t[debug_capacity]();
        if (output->debug_detections == nullptr) {
            output->debug_detection_capacity = 0;
            return;
        }
        output->debug_detection_capacity = static_cast<int>(debug_capacity);
    }
}

bool append_detection(PipelineOutput* output, const std::array<FloatPoint, 4>& polygon, const std::string& text) {
    if (output == nullptr || output->detection_count >= output->detection_capacity) {
        return false;
    }
    auto& detection = output->detections[output->detection_count++];
    for (int p = 0; p < 4; ++p) {
        detection.polygon[p].x = polygon[p].x;
        detection.polygon[p].y = polygon[p].y;
    }
    detection.text = dup_string(text);
    if (detection.text == nullptr) {
        --output->detection_count;
        return false;
    }
    return true;
}

bool append_debug_detection(
    PipelineOutput* output,
    const std::array<FloatPoint, 4>& polygon,
    const std::string& text,
    float score,
    bool accepted
) {
    if (output == nullptr || output->debug_detection_count >= output->debug_detection_capacity) {
        return false;
    }
    auto& detection = output->debug_detections[output->debug_detection_count++];
    for (int p = 0; p < 4; ++p) {
        detection.polygon[p].x = polygon[p].x;
        detection.polygon[p].y = polygon[p].y;
    }
    detection.text = dup_string(text);
    if (detection.text == nullptr) {
        --output->debug_detection_count;
        return false;
    }
    detection.score = score;
    detection.accepted = accepted ? 1 : 0;
    return true;
}

void reset_pipeline_output(PipelineOutput* output) {
    if (output == nullptr) {
        return;
    }
    output->detections = nullptr;
    output->detection_count = 0;
    output->detection_capacity = 0;
    output->debug_detections = nullptr;
    output->debug_detection_count = 0;
    output->debug_detection_capacity = 0;
}

void free_pipeline_output(PipelineOutput* output) {
    if (output == nullptr) {
        return;
    }
    if (output->detections != nullptr) {
        for (int i = 0; i < output->detection_count; ++i) {
            delete[] output->detections[i].text;
            output->detections[i].text = nullptr;
        }
        delete[] output->detections;
    }
    if (output->debug_detections != nullptr) {
        for (int i = 0; i < output->debug_detection_count; ++i) {
            delete[] output->debug_detections[i].text;
            output->debug_detections[i].text = nullptr;
        }
        delete[] output->debug_detections;
    }
    reset_pipeline_output(output);
}

buzhi_ocr_result_t* build_native_result(PipelineOutput&& output, bool include_debug) {
    auto* result = new buzhi_ocr_result_t{};
    result->detections = output.detections;
    result->detection_count = output.detection_count;
    if (include_debug) {
        result->debug_detections = output.debug_detections;
        result->debug_detection_count = output.debug_detection_count;
    } else if (output.debug_detections != nullptr) {
        for (int i = 0; i < output.debug_detection_count; ++i) {
            delete[] output.debug_detections[i].text;
        }
        delete[] output.debug_detections;
    }
    reset_pipeline_output(&output);
    return result;
}

extern "C" void buzhi_ocr_free_string(char* s) {
    delete[] s;
}

extern "C" void buzhi_ocr_free_result(buzhi_ocr_result_t* result) {
    if (result == nullptr) {
        return;
    }
    if (result->detections != nullptr) {
        for (int i = 0; i < result->detection_count; ++i) {
            delete[] result->detections[i].text;
        }
        delete[] result->detections;
    }
    if (result->debug_detections != nullptr) {
        for (int i = 0; i < result->debug_detection_count; ++i) {
            delete[] result->debug_detections[i].text;
        }
        delete[] result->debug_detections;
    }
    delete result;
}
