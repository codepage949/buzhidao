#include "bridge_rec_decode.h"

#include "bridge_utils.h"

#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>

std::pair<std::string, float> decode_ctc(
    const float* pred,
    int time_steps,
    int num_classes,
    const std::vector<std::string>& dict
) {
    if (pred == nullptr || time_steps <= 0 || num_classes <= 0) {
        return {"", 0.0f};
    }
    int prev = -1;
    double score_sum = 0.0;
    int score_count = 0;
    std::string text;
    const bool debug = debug_enabled();
    std::vector<int> best_indices;
    std::vector<float> best_scores;
    if (debug) {
        best_indices.reserve(static_cast<size_t>(time_steps));
        best_scores.reserve(static_cast<size_t>(time_steps));
    }

    for (int t = 0; t < time_steps; ++t) {
        const size_t base = static_cast<size_t>(t) * num_classes;
        int best = 0;
        float best_score = pred[base];
        for (int c = 1; c < num_classes; ++c) {
            const float score = pred[base + c];
            if (score > best_score) {
                best_score = score;
                best = c;
            }
        }
        if (debug) {
            best_indices.push_back(best);
            best_scores.push_back(best_score);
        }
        if (best != 0 && best != prev) {
            const int dict_idx = best - 1;
            if (dict_idx >= 0 && dict_idx < static_cast<int>(dict.size())) {
                text += dict[dict_idx];
                score_sum += best_score;
                ++score_count;
            }
        }
        prev = best;
    }

    const float score = score_count > 0 ? static_cast<float>(score_sum / score_count) : 0.0f;
    bool dump_all_ctc = false;
    if (debug) {
        const char* dump_all_ctc_raw = std::getenv("BUZHIDAO_PADDLE_FFI_DEBUG_CTC_ALL");
        dump_all_ctc =
            dump_all_ctc_raw != nullptr &&
            dump_all_ctc_raw[0] != '\0' &&
            std::strcmp(dump_all_ctc_raw, "0") != 0 &&
            std::strcmp(dump_all_ctc_raw, "false") != 0 &&
            std::strcmp(dump_all_ctc_raw, "FALSE") != 0;
    }
    if (debug && (text.empty() || dump_all_ctc)) {
        std::ostringstream os;
        os << "decode_ctc text='" << text << "' top_idx=";
        for (size_t i = 0; i < best_indices.size() && i < 24; ++i) {
            if (i > 0) {
                os << ",";
            }
            os << best_indices[i] << "@" << std::fixed << std::setprecision(4) << best_scores[i];
            if (best_indices[i] > 0) {
                const int dict_idx = best_indices[i] - 1;
                if (dict_idx >= 0 && dict_idx < static_cast<int>(dict.size())) {
                    os << ":" << dict[dict_idx];
                }
            }
        }
        profile_log(os.str());
        debug_log(os.str());
    }
    return {text, score};
}

std::pair<std::string, float> decode_ctc(
    const std::vector<float>& pred,
    int time_steps,
    int num_classes,
    const std::vector<std::string>& dict
) {
    if (pred.empty()) {
        return {"", 0.0f};
    }
    return decode_ctc(pred.data(), time_steps, num_classes, dict);
}
