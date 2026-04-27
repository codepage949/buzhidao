#include "bridge_predictor.h"

#include "bridge_utils.h"

#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

std::vector<int> infer_shape_as_ints(const std::vector<int64_t>& shape64) {
    std::vector<int> out;
    out.reserve(shape64.size());
    for (const auto dim : shape64) {
        if (dim > 0) {
            out.push_back(static_cast<int>(dim));
        } else {
            out.push_back(0);
        }
    }
    return out;
}

template <typename T>
std::vector<int> shape_to_ints(const std::vector<T>& shape) {
    std::vector<int> out;
    out.reserve(shape.size());
    for (auto d : shape) {
        if (d > 0) {
            out.push_back(static_cast<int>(d));
        } else {
            out.push_back(0);
        }
    }
    return out;
}

size_t shape_elements(const std::vector<int>& shape) {
    size_t n = 1;
    for (const auto dim : shape) {
        if (dim <= 0) {
            return 0;
        }
        n *= static_cast<size_t>(dim);
    }
    return n;
}

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
PredictorIoNames resolve_predictor_io_names(
    const std::shared_ptr<paddle_infer::Predictor>& predictor
) {
    static std::mutex cache_mutex;
    static std::unordered_map<const void*, PredictorIoNames> cache;
    if (!predictor) {
        return {};
    }
    const void* const key = predictor.get();
    {
        const std::lock_guard<std::mutex> lock(cache_mutex);
        const auto it = cache.find(key);
        if (it != cache.end()) {
            return it->second;
        }
    }

    PredictorIoNames names;
    const auto input_names = predictor->GetInputNames();
    if (!input_names.empty()) {
        names.input_name = input_names.front();
    }
    const auto output_names = predictor->GetOutputNames();
    if (!output_names.empty()) {
        names.output_name = output_names.front();
    }
    if (!names.input_name.empty() && !names.output_name.empty()) {
        const std::lock_guard<std::mutex> lock(cache_mutex);
        cache.emplace(key, names);
    }
    return names;
}

bool run_predictor(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const std::vector<float>& input,
    const std::vector<int>& shape,
    std::vector<float>& output,
    std::vector<int>& output_shape,
    std::string* err
) {
    try {
        if (!predictor) {
            if (err != nullptr) {
                *err = "predictor가 없습니다";
            }
            return false;
        }
        const PredictorIoNames io_names = resolve_predictor_io_names(predictor);
        if (io_names.input_name.empty()) {
            if (err != nullptr) {
                *err = "모델 입력 노드가 비어 있습니다";
            }
            return false;
        }
        if (debug_enabled()) {
            std::string shape_str;
            for (size_t i = 0; i < shape.size(); ++i) {
                if (i > 0) {
                    shape_str += "x";
                }
                shape_str += std::to_string(shape[i]);
            }
            debug_log("run_predictor input_name=" + io_names.input_name +
                      ", shape=" + shape_str +
                      ", input_len=" + std::to_string(input.size()));
            if (!input.empty()) {
                debug_log("run_predictor input sample=" +
                          std::to_string(input[0]) + ", " +
                          std::to_string(input[input.size() / 2]) + ", " +
                          std::to_string(input.back()));
            }
        }

        auto input_handle = predictor->GetInputHandle(io_names.input_name);
        if (!input_handle) {
            if (err != nullptr) {
                *err = "입력 핸들을 확보하지 못했습니다";
            }
            return false;
        }
        input_handle->Reshape(shape);
        input_handle->CopyFromCpu(input.data());

        if (!predictor->Run()) {
            if (err != nullptr) {
                *err = "predictor 실행 실패";
            }
            return false;
        }

        if (io_names.output_name.empty()) {
            if (err != nullptr) {
                *err = "모델 출력 노드가 비어 있습니다";
            }
            return false;
        }
        if (debug_enabled()) {
            debug_log("run_predictor output_name=" + io_names.output_name);
        }

        auto output_handle = predictor->GetOutputHandle(io_names.output_name);
        if (!output_handle) {
            if (err != nullptr) {
                *err = "출력 핸들을 확보하지 못했습니다";
            }
            return false;
        }
        auto output_shape64 = output_handle->shape();
        output_shape = shape_to_ints(output_shape64);
        const size_t n = shape_elements(output_shape);
        if (n == 0) {
            output.clear();
            return true;
        }
        output.resize(n);
        output_handle->CopyToCpu(output.data());
        return true;
    } catch (const std::exception& ex) {
        if (err != nullptr) {
            *err = std::string("predictor 실행 예외: ") + ex.what();
        }
        return false;
    }
}

bool run_predictor_into_buffer(
    const std::shared_ptr<paddle_infer::Predictor>& predictor,
    const float* input_data,
    size_t input_len,
    const std::vector<int>& shape,
    FloatScratchBuffer* output,
    size_t* output_len,
    std::vector<int>& output_shape,
    std::string* err
) {
    try {
        if (!predictor) {
            if (err != nullptr) {
                *err = "predictor가 없습니다";
            }
            return false;
        }
        if (output == nullptr || output_len == nullptr) {
            if (err != nullptr) {
                *err = "출력 scratch buffer가 없습니다";
            }
            return false;
        }
        const PredictorIoNames io_names = resolve_predictor_io_names(predictor);
        if (io_names.input_name.empty()) {
            if (err != nullptr) {
                *err = "모델 입력 노드가 비어 있습니다";
            }
            return false;
        }
        if (debug_enabled()) {
            std::string shape_str;
            for (size_t i = 0; i < shape.size(); ++i) {
                if (i > 0) {
                    shape_str += "x";
                }
                shape_str += std::to_string(shape[i]);
            }
            debug_log("run_predictor input_name=" + io_names.input_name +
                      ", shape=" + shape_str +
                      ", input_len=" + std::to_string(input_len));
            if (input_data != nullptr && input_len > 0) {
                debug_log("run_predictor input sample=" +
                          std::to_string(input_data[0]) + ", " +
                          std::to_string(input_data[input_len / 2]) + ", " +
                          std::to_string(input_data[input_len - 1]));
            }
        }

        auto input_handle = predictor->GetInputHandle(io_names.input_name);
        if (!input_handle) {
            if (err != nullptr) {
                *err = "입력 핸들을 확보하지 못했습니다";
            }
            return false;
        }
        input_handle->Reshape(shape);
        input_handle->CopyFromCpu(input_data);

        if (!predictor->Run()) {
            if (err != nullptr) {
                *err = "predictor 실행 실패";
            }
            return false;
        }

        if (io_names.output_name.empty()) {
            if (err != nullptr) {
                *err = "모델 출력 노드가 비어 있습니다";
            }
            return false;
        }
        if (debug_enabled()) {
            debug_log("run_predictor output_name=" + io_names.output_name);
        }

        auto output_handle = predictor->GetOutputHandle(io_names.output_name);
        if (!output_handle) {
            if (err != nullptr) {
                *err = "출력 핸들을 확보하지 못했습니다";
            }
            return false;
        }
        auto output_shape64 = output_handle->shape();
        output_shape = shape_to_ints(output_shape64);
        const size_t n = shape_elements(output_shape);
        *output_len = n;
        if (n == 0) {
            return true;
        }
        if (!output->ensure(n, err)) {
            return false;
        }
        output_handle->CopyToCpu(output->get());
        return true;
    } catch (const std::exception& ex) {
        if (err != nullptr) {
            *err = std::string("predictor 실행 예외: ") + ex.what();
        }
        return false;
    }
}

#endif

int find_rec_layout(const std::vector<int>& shape, int& time_steps, int& num_classes) {
    time_steps = 0;
    num_classes = 0;
    if (shape.size() == 4) {
        // Accept layouts where the class axis is still the last axis and one inner axis is singleton.
        if (shape[2] == 1 && shape[1] > 0 && shape[3] > 0) {
            time_steps = shape[1];
            num_classes = shape[3];
            return 2; // [N,T,1,C]
        }
        if (shape[1] == 1 && shape[2] > 0 && shape[3] > 0) {
            time_steps = shape[2];
            num_classes = shape[3];
            return 2; // [N,1,T,C]
        }
    }
    if (shape.size() == 3) {
        // sidecar CTCLabelDecode uses preds.argmax(axis=-1), so class axis must be the last axis.
        time_steps = shape[1];
        num_classes = shape[2];
        return 2; // [N,T,C]
    }
    if (shape.size() == 2) {
        // [T, C]
        num_classes = shape[1];
        time_steps = shape[0];
        return 2;
    }
    return 0;
}
