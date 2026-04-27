#include "bridge_utils.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;

bool debug_enabled() {
    const char* raw = std::getenv("BUZHIDAO_PADDLE_FFI_TRACE");
    if (raw == nullptr || raw[0] == '\0') {
        return false;
    }
    const auto token = to_lower_ascii(normalize_hint(raw));
    return token == "1" || token == "true" || token == "on" || token == "yes";
}

bool profile_stages_enabled() {
    const char* raw = std::getenv("BUZHIDAO_PADDLE_FFI_PROFILE_STAGES");
    if (raw == nullptr || raw[0] == '\0') {
        return false;
    }
    const auto token = to_lower_ascii(normalize_hint(raw));
    return token == "1" || token == "true" || token == "on" || token == "yes";
}

double elapsed_ms_since(const std::chrono::steady_clock::time_point& started) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started
    ).count();
}

std::string debug_dump_dir() {
    const char* raw = std::getenv("BUZHIDAO_PADDLE_FFI_DUMP_DIR");
    if (raw == nullptr || raw[0] == '\0') {
        return {};
    }
    const std::string normalized = normalize_hint(raw);
    std::error_code ec;
    fs::create_directories(fs::path(normalized), ec);
    if (ec) {
        std::cerr << "[buzhi_ocr] dump dir create failed: " << normalized
                  << ", error=" << ec.message() << std::endl;
        return {};
    }
    return normalized;
}

void debug_log(const std::string& message) {
    if (!debug_enabled()) {
        return;
    }
    std::cerr << "[buzhi_ocr] " << message << std::endl;
    const char* tmp_dir = std::getenv("TMPDIR");
    const std::string log_path =
        std::string((tmp_dir != nullptr && tmp_dir[0] != '\0') ? tmp_dir : "/tmp") +
        "/buzhi-ocr-ffi-debug.log";
    std::ofstream log_file(log_path, std::ios::app);
    if (log_file) {
        log_file << "[buzhi_ocr] " << message << std::endl;
    }
}

void profile_log(const std::string& message) {
    if (!profile_stages_enabled()) {
        return;
    }
    std::cerr << "[buzhi_ocr_profile] " << message << std::endl;
    const char* tmp_dir = std::getenv("TMPDIR");
    const std::string log_path =
        std::string((tmp_dir != nullptr && tmp_dir[0] != '\0') ? tmp_dir : "/tmp") +
        "/buzhi-ocr-ffi-profile.log";
    std::ofstream log_file(log_path, std::ios::app);
    if (log_file) {
        log_file << "[buzhi_ocr_profile] " << message << std::endl;
    }
}

std::string trim(std::string value) {
    auto left = std::find_if_not(value.begin(), value.end(), [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    });
    auto right = std::find_if_not(value.rbegin(), value.rend(), [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    }).base();
    if (left >= right) {
        return {};
    }
    return std::string(left, right);
}

std::string normalize_hint(std::string value) {
    auto normalized = trim(std::move(value));
    if (normalized.empty()) {
        return {};
    }
    normalized = to_lower_ascii(std::move(normalized));
    return normalized;
}

std::string to_lower_ascii(std::string value) {
    for (auto& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

bool parse_env_float(const char* value, float* out) {
    if (value == nullptr || out == nullptr) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(value, &end);
    if (errno != 0 || end == value || *end != '\0') {
        return false;
    }
    *out = parsed;
    return true;
}

bool parse_env_int(const char* value, int* out) {
    if (value == nullptr || out == nullptr) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return false;
    }
    *out = static_cast<int>(parsed);
    return true;
}

std::unordered_set<std::string> parse_env_csv_set(const char* value) {
    std::unordered_set<std::string> out;
    if (value == nullptr) {
        return out;
    }
    std::string current;
    auto flush = [&out, &current]() {
        if (current.empty()) {
            return;
        }
        size_t start = 0;
        while (start < current.size() && std::isspace(static_cast<unsigned char>(current[start])) != 0) {
            ++start;
        }
        size_t end = current.size();
        while (end > start && std::isspace(static_cast<unsigned char>(current[end - 1])) != 0) {
            --end;
        }
        if (start < end) {
            out.insert(current.substr(start, end - start));
        }
        current.clear();
    };
    for (const char* p = value; *p != '\0'; ++p) {
        if (*p == ',') {
            flush();
            continue;
        }
        current.push_back(*p);
    }
    flush();
    return out;
}

bool parse_env_bool(const char* value, bool* out) {
    if (value == nullptr || out == nullptr) {
        return false;
    }
    const auto normalized = to_lower_ascii(trim(value));
    if (
        normalized == "1" ||
        normalized == "true" ||
        normalized == "yes" ||
        normalized == "on" ||
        normalized == "y"
    ) {
        *out = true;
        return true;
    }
    if (
        normalized == "0" ||
        normalized == "false" ||
        normalized == "no" ||
        normalized == "off" ||
        normalized == "n"
    ) {
        *out = false;
        return true;
    }
    return false;
}

std::string trim_or_empty(const std::string& value) {
    return trim(value);
}

void append_utf8_codepoint(std::string& out, uint32_t codepoint) {
    if (codepoint <= 0x7Fu) {
        out.push_back(static_cast<char>(codepoint));
        return;
    }
    if (codepoint <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | ((codepoint >> 6) & 0x1Fu)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
        return;
    }
    if (codepoint <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | ((codepoint >> 12) & 0x0Fu)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
        return;
    }
    out.push_back(static_cast<char>(0xF0u | ((codepoint >> 18) & 0x07u)));
    out.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
}

bool parse_hex4(const std::string& text, size_t pos, uint32_t& value) {
    if (pos + 4 > text.size()) {
        return false;
    }
    value = 0;
    for (size_t j = 0; j < 4; ++j) {
        const char ch = text[pos + j];
        value <<= 4;
        if (ch >= '0' && ch <= '9') {
            value |= static_cast<uint32_t>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            value |= static_cast<uint32_t>(10 + ch - 'a');
        } else if (ch >= 'A' && ch <= 'F') {
            value |= static_cast<uint32_t>(10 + ch - 'A');
        } else {
            return false;
        }
    }
    return true;
}

std::string parse_json_string(const std::string& text, size_t& i) {
    if (i >= text.size() || text[i] != '\"') {
        return {};
    }
    ++i;
    std::string out;
    for (; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '\\') {
            ++i;
            if (i >= text.size()) {
                return {};
            }
            const char esc = text[i];
            switch (esc) {
            case '\"':
            case '\\':
            case '/':
                out.push_back(esc);
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                uint32_t codepoint = 0;
                if (!parse_hex4(text, i + 1, codepoint)) {
                    return {};
                }
                i += 4;
                if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
                    if (i + 6 >= text.size() || text[i + 1] != '\\' || text[i + 2] != 'u') {
                        return {};
                    }
                    uint32_t low = 0;
                    if (!parse_hex4(text, i + 3, low) || low < 0xDC00u || low > 0xDFFFu) {
                        return {};
                    }
                    i += 6;
                    codepoint = 0x10000u + (((codepoint - 0xD800u) << 10) | (low - 0xDC00u));
                }
                append_utf8_codepoint(out, codepoint);
                break;
            }
            default:
                out.push_back(esc);
                break;
            }
            continue;
        }
        if (ch == '\"') {
            ++i;
            return out;
        }
        out.push_back(ch);
    }
    return {};
}

std::string extract_json_block(const std::string& text, size_t start) {
    if (start >= text.size()) {
        return {};
    }
    const char open = text[start];
    const char close = open == '{' ? '}' : ']';
    if (open != '{' && open != '[') {
        return {};
    }
    int depth = 0;
    bool in_str = false;
    bool escape = false;
    for (size_t i = start; i < text.size(); ++i) {
        const char ch = text[i];
        if (in_str) {
            if (escape) {
                escape = false;
                continue;
            }
            if (ch == '\\') {
                escape = true;
                continue;
            }
            if (ch == '\"') {
                in_str = false;
            }
            continue;
        }
        if (ch == '\"') {
            in_str = true;
            continue;
        }
        if (ch == open) {
            ++depth;
            continue;
        }
        if (ch == close) {
            --depth;
            if (depth == 0) {
                return text.substr(start, i - start + 1);
            }
        }
    }
    return {};
}

std::string extract_json_key_value(
    const std::string& text,
    const std::string& key,
    bool preserve_string
) {
    const std::string quoted_key = "\"" + key + "\"";
    size_t pos = 0;
    while (true) {
        pos = text.find(quoted_key, pos);
        if (pos == std::string::npos) {
            return {};
        }

        const size_t colon = text.find(':', pos + quoted_key.size());
        if (colon == std::string::npos) {
            return {};
        }

        size_t i = colon + 1;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
            ++i;
        }
        if (i >= text.size()) {
            return {};
        }

        if (text[i] == '{' || text[i] == '[') {
            return extract_json_block(text, i);
        }
        if (text[i] == '\"') {
            const size_t start = i;
            auto parsed = parse_json_string(text, i);
            return preserve_string ? parsed : text.substr(start, i - start);
        }

        size_t end = i;
        bool in_str = false;
        bool esc = false;
        int depth_obj = 0;
        int depth_arr = 0;
        while (end < text.size()) {
            const char ch = text[end];
            if (in_str) {
                if (esc) {
                    esc = false;
                } else if (ch == '\\') {
                    esc = true;
                } else if (ch == '\"') {
                    in_str = false;
                }
            } else if (ch == '\"') {
                in_str = true;
            } else if (ch == '{') {
                ++depth_obj;
            } else if (ch == '}') {
                if (depth_obj > 0) {
                    --depth_obj;
                }
            } else if (ch == '[') {
                ++depth_arr;
            } else if (ch == ']') {
                if (depth_arr > 0) {
                    --depth_arr;
                }
            } else if (ch == ',' && depth_obj == 0 && depth_arr == 0) {
                break;
            }
            ++end;
        }
        return trim_or_empty(text.substr(i, end - i));
    }
}

std::vector<std::string> parse_json_array(const std::string& array_text) {
    const auto trimmed = trim_or_empty(array_text);
    if (trimmed.size() < 2 || trimmed.front() != '[' || trimmed.back() != ']') {
        return {};
    }
    std::vector<std::string> values;
    size_t i = 1;
    while (i + 1 < trimmed.size()) {
        while (i < trimmed.size() - 1 && (trimmed[i] == ',' || std::isspace(static_cast<unsigned char>(trimmed[i])) != 0)) {
            ++i;
        }
        if (i + 1 >= trimmed.size()) {
            break;
        }

        if (trimmed[i] == '\"') {
            const auto item = parse_json_string(trimmed, i);
            values.push_back(item);
            while (i < trimmed.size() && trimmed[i] != ',' && trimmed[i] != ']') {
                ++i;
            }
            continue;
        }

        if (trimmed[i] == '{' || trimmed[i] == '[') {
            const auto block = extract_json_block(trimmed, i);
            if (block.empty()) {
                return {};
            }
            values.push_back(block);
            i += block.size();
            continue;
        }

        const size_t start = i;
        bool in_str = false;
        bool esc = false;
        int depth_obj = 0;
        int depth_arr = 0;
        while (i < trimmed.size() - 1) {
            const char ch = trimmed[i];
            if (in_str) {
                if (esc) {
                    esc = false;
                    ++i;
                    continue;
                }
                if (ch == '\\') {
                    esc = true;
                    ++i;
                    continue;
                }
                if (ch == '\"') {
                    in_str = false;
                }
                ++i;
                continue;
            }
            if (ch == '\"') {
                in_str = true;
                ++i;
                continue;
            }
            if (ch == '{') {
                ++depth_obj;
            } else if (ch == '}') {
                if (depth_obj > 0) {
                    --depth_obj;
                }
            } else if (ch == '[') {
                ++depth_arr;
            } else if (ch == ']') {
                if (depth_arr > 0) {
                    --depth_arr;
                }
            } else if (ch == ',' && depth_obj == 0 && depth_arr == 0) {
                break;
            }
            ++i;
        }
        values.push_back(trim_or_empty(trimmed.substr(start, i - start)));
        if (i < trimmed.size() && trimmed[i] != ',') {
            ++i;
        }
    }
    return values;
}

bool parse_float_list(const std::string& text, const std::string& key, std::array<float, 3>& out) {
    const auto raw = extract_json_key_value(text, key);
    const auto values = parse_json_array(raw);
    if (values.size() < 3) {
        return false;
    }
    for (size_t i = 0; i < 3; ++i) {
        if (!parse_float(values[i], &out[i])) {
            return false;
        }
    }
    return true;
}

bool parse_int_list(const std::string& text, const std::string& key, std::array<int, 3>& out) {
    const auto raw = extract_json_key_value(text, key);
    const auto values = parse_json_array(raw);
    if (values.size() < 3) {
        return false;
    }
    for (size_t i = 0; i < 3; ++i) {
        if (!parse_int(values[i], &out[i])) {
            return false;
        }
    }
    return true;
}

float parse_scale_value(const std::string& value, float fallback) {
    const auto trimmed = trim_or_empty(value);
    float parsed = fallback;
    if (parse_float(trimmed, &parsed)) {
        return parsed;
    }
    const auto slash = trimmed.find('/');
    if (slash != std::string::npos) {
        float left = 0.0f;
        float right = 1.0f;
        if (parse_float(trimmed.substr(0, slash), &left) &&
            parse_float(trimmed.substr(slash + 1), &right) &&
            right != 0.0f) {
            return left / right;
        }
    }
    return fallback;
}

bool parse_float(const std::string& token, float* out) {
    if (out == nullptr) {
        return false;
    }
    const auto t = trim_or_empty(token);
    if (t.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(t.c_str(), &end);
    if (errno != 0 || end == t.c_str() || *end != '\0') {
        return false;
    }
    *out = parsed;
    return true;
}

bool parse_int(const std::string& token, int* out) {
    if (out == nullptr) {
        return false;
    }
    const auto t = trim_or_empty(token);
    if (t.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(t.c_str(), &end, 10);
    if (errno != 0 || end == t.c_str() || *end != '\0') {
        return false;
    }
    *out = static_cast<int>(parsed);
    return true;
}

std::string read_text_file(const fs::path& path) {
    std::ifstream in(path);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string lookup_json_key(
    const std::string& text,
    const std::vector<std::string>& keys,
    bool preserve_string
) {
    for (const auto& key : keys) {
        const auto value = extract_json_key_value(text, key, preserve_string);
        if (!value.empty()) {
            return value;
        }
    }
    return {};
}

std::string json_escape(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size() + text.size() / 4);
    for (const auto ch : text) {
        switch (ch) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped.push_back(ch);
        }
    }
    return escaped;
}
