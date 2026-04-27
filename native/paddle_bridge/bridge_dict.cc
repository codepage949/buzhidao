#include "bridge_dict.h"

#include "bridge_fs.h"
#include "bridge_utils.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

constexpr char RAW_DICT_HINT[] =
    "다음 중 하나로 사전이 있어야 합니다: rec_dict.txt, ppocr_keys_v1.txt, ppocr_keys_v2.txt.";

bool is_json_like_dict_name(const std::string& name) {
    const std::string lower = to_lower_ascii(name);
    return lower.find("dict") != std::string::npos ||
        lower.find("character") != std::string::npos ||
        lower.find("key") != std::string::npos;
}

std::vector<std::string> split_lines(const fs::path& path, std::string* err) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    if (!in) {
        if (err != nullptr) {
            *err = "사전 파일을 열 수 없습니다: " + path.string();
        }
        return {};
    }
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    if (lines.empty() && err != nullptr) {
        *err = "사전 파일이 비어있습니다: " + path.string();
    }
    return lines;
}

bool append_char_dict_from_json(const std::string& text, std::vector<std::string>& dict) {
    const std::string key = "\"character_dict\"";
    const size_t key_pos = text.find(key);
    if (key_pos == std::string::npos) {
        return false;
    }
    size_t bracket_pos = text.find('[', key_pos);
    if (bracket_pos == std::string::npos) {
        return false;
    }
    ++bracket_pos;
    int depth = 1;
    bool in_string = false;
    bool escape = false;
    std::string current;
    for (size_t i = bracket_pos; i < text.size(); ++i) {
        const char ch = text[i];
        if (in_string) {
            if (escape) {
                current.push_back(ch);
                escape = false;
            } else if (ch == '\\') {
                escape = true;
            } else if (ch == '"') {
                const std::string v = trim(current);
                if (!v.empty()) {
                    dict.push_back(v);
                }
                current.clear();
                in_string = false;
            } else {
                current.push_back(ch);
            }
        } else {
            if (ch == '"') {
                in_string = true;
            } else if (ch == '[') {
                ++depth;
            } else if (ch == ']') {
                --depth;
                if (depth == 0) {
                    return !dict.empty();
                }
            }
        }
    }
    return false;
}

bool append_char_dict_from_yaml(const std::string& text, std::vector<std::string>& dict) {
    std::istringstream stream(text);
    std::string line;
    bool in_dict = false;
    size_t dict_indent = 0;
    while (std::getline(stream, line)) {
        const auto trimmed = trim(line);
        if (!in_dict) {
            if (trimmed == "character_dict:") {
                in_dict = true;
                dict_indent = line.find_first_not_of(" \t");
                if (dict_indent == std::string::npos) {
                    dict_indent = 0;
                }
            }
            continue;
        }

        const auto indent = line.find_first_not_of(" \t");
        if (indent == std::string::npos) {
            continue;
        }
        std::string item = trim(line.substr(indent));
        if (indent <= dict_indent && (item.empty() || item[0] != '-')) {
            break;
        }
        if (item.empty() || item[0] != '-') {
            continue;
        }
        item = trim(item.substr(1));
        if (item.empty()) {
            continue;
        }
        if (item.size() >= 2) {
            const char first = item.front();
            const char last = item.back();
            if ((first == '\'' && last == '\'') || (first == '"' && last == '"')) {
                item = item.substr(1, item.size() - 2);
            }
        }
        if (item == "''''") {
            item = "'";
        } else if (item == "\\") {
            item = "\\";
        }
        dict.push_back(item);
    }
    return !dict.empty();
}

std::vector<std::string> parse_dict_from_structured_file(const fs::path& path, std::string* err) {
    std::ifstream in(path);
    if (!in) {
        if (err != nullptr) {
            *err = "사전 메타 파일을 열 수 없습니다: " + path.string();
        }
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    std::vector<std::string> dict;
    const auto ext = to_lower_ascii(path.extension().string());
    const bool parsed = (ext == ".json")
        ? append_char_dict_from_json(text, dict)
        : append_char_dict_from_yaml(text, dict);
    if (!parsed) {
        return {};
    }
    if (std::find(dict.begin(), dict.end(), std::string(" ")) == dict.end()) {
        dict.push_back(" ");
    }
    return dict;
}

bool parse_candidate_dict(const fs::path& candidate, std::vector<std::string>& dict) {
    if (!file_exists(candidate)) {
        return false;
    }
    if (candidate.extension() == ".txt") {
        std::string err;
        dict = split_lines(candidate, &err);
        if (!dict.empty() && std::find(dict.begin(), dict.end(), std::string(" ")) == dict.end()) {
            dict.push_back(" ");
        }
        return !dict.empty();
    }
    if (candidate.extension() == ".json" || candidate.extension() == ".yaml" ||
        candidate.extension() == ".yml") {
        std::string err;
        dict = parse_dict_from_structured_file(candidate, &err);
        return !dict.empty();
    }
    return false;
}

std::vector<std::string> load_recognition_dict(const fs::path& model_dir, std::string* err) {
    static const std::vector<std::string> direct_candidates = {
        "rec_dict.txt",
        "ppocr_keys_v1.txt",
        "ppocr_keys_v2.txt",
        "dict.txt",
        "character_dict.txt",
        "label.txt",
    };
    for (const auto& name : direct_candidates) {
        const auto path = model_dir / name;
        std::vector<std::string> dict;
        if (parse_candidate_dict(path, dict)) {
            return dict;
        }
    }

    for (const auto& name : {
             "config.json",
             "inference.json",
             "inference_config.json",
             "inference.yaml",
             "inference.yml"}) {
        const auto json_path = model_dir / name;
        std::string parse_err;
        const auto dict = parse_dict_from_structured_file(json_path, &parse_err);
        if (!dict.empty()) {
            return dict;
        }
    }

    for (const auto& entry : fs::recursive_directory_iterator(model_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const fs::path p = entry.path();
        if (p.extension() != ".txt") {
            continue;
        }
        const std::string stem = p.filename().string();
        if (!is_json_like_dict_name(stem)) {
            continue;
        }
        std::vector<std::string> dict;
        if (parse_candidate_dict(p, dict)) {
            return dict;
        }
    }

    if (err != nullptr) {
        *err = RAW_DICT_HINT;
    }
    return {};
}

bool validate_recognition_dict(std::vector<std::string>* dict, std::string* err) {
    if (dict == nullptr) {
        if (err != nullptr) {
            *err = "rec_dict 버퍼가 없습니다";
        }
        return false;
    }
    std::vector<std::string> cleaned;
    cleaned.reserve(dict->size());
    bool has_non_empty = false;
    for (const auto& entry : *dict) {
        if (entry.empty()) {
            continue;
        }
        if (entry != " ") {
            has_non_empty = true;
        }
        cleaned.push_back(entry);
    }
    if (!has_non_empty) {
        if (err != nullptr) {
            *err = "rec_dict에 유효한 문자가 없습니다";
        }
        return false;
    }
    if (std::find(cleaned.begin(), cleaned.end(), std::string(" ")) == cleaned.end()) {
        cleaned.push_back(" ");
    }
    *dict = std::move(cleaned);
    return true;
}
