#ifndef BUZHIDAO_PADDLE_BRIDGE_UTILS_H
#define BUZHIDAO_PADDLE_BRIDGE_UTILS_H

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

std::string trim(std::string value);
std::string normalize_hint(std::string value);
std::string to_lower_ascii(std::string value);

bool debug_enabled();
bool profile_stages_enabled();
double elapsed_ms_since(const std::chrono::steady_clock::time_point& started);
std::string debug_dump_dir();
void debug_log(const std::string& message);
void profile_log(const std::string& message);

template <typename Builder>
void debug_log_lazy(Builder&& builder) {
    if (!debug_enabled()) {
        return;
    }
    debug_log(builder());
}

bool parse_env_float(const char* value, float* out);
bool parse_env_int(const char* value, int* out);
bool parse_env_bool(const char* value, bool* out);
std::unordered_set<std::string> parse_env_csv_set(const char* value);

std::string read_text_file(const std::filesystem::path& path);
std::string trim_or_empty(const std::string& value);
float parse_scale_value(const std::string& value, float fallback);
std::string json_escape(const std::string& text);
std::string lookup_json_key(
    const std::string& text,
    const std::vector<std::string>& keys,
    bool preserve_string = false
);
std::string extract_json_key_value(
    const std::string& text,
    const std::string& key,
    bool preserve_string = false
);
std::string parse_json_string(const std::string& text, size_t& i);
std::string extract_json_block(
    const std::string& text,
    size_t start
);
std::vector<std::string> parse_json_array(
    const std::string& array_text
);
bool parse_float_list(
    const std::string& text,
    const std::string& key,
    std::array<float, 3>& out
);
bool parse_int_list(
    const std::string& text,
    const std::string& key,
    std::array<int, 3>& out
);
bool parse_float(
    const std::string& token,
    float* out
);
bool parse_int(
    const std::string& token,
    int* out
);

#endif
