#include "bridge_model.h"
#include "bridge_env.h"

#include "bridge_fs.h"
#include "bridge_utils.h"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <vector>

namespace fs = std::filesystem;

std::vector<std::string> find_stem_aliases(const std::string& stem) {
    if (stem == "det") {
        return {
            "det",
            "textdet",
            "text_det",
            "detection",
            "textdetv",
            "text_detection",
        };
    }
    if (stem == "cls") {
        return {
            "cls",
            "textcls",
            "textline",
            "orientation",
            "angle",
            "textorientation",
        };
    }
    return {
        "rec",
        "textrec",
        "text_rec",
        "recognition",
        "textrecg",
        "text_recog",
    };
}

std::vector<std::string> find_stem_family_suffixes(const std::string& stem) {
    if (stem == "det") {
        return {
            "_textdetv",
            "_text_detection",
            "_textdet",
            "_text_det",
            "_det",
            "_detect",
        };
    }
    if (stem == "cls") {
        return {
            "_textorientation",
            "_textline_ori",
            "_text_line_ori",
            "_textline",
            "_orientation",
            "_angle",
            "_cls",
        };
    }
    return {
        "_textrecog",
        "_text_recog",
        "_textrec",
        "_text_rec",
        "_recognition",
        "_mobile_rec",
        "_rec",
    };
}

std::string resolve_preferred_lang() {
    const char* raw = std::getenv(buzhidao_env::kFfiSource);
    if (raw == nullptr) {
        return "en";
    }
    const std::string value = normalize_hint(raw);
    if (value.empty()) {
        return "en";
    }
    if (value == "en" || value == "eng" || value == "english") {
        return "en";
    }
    if (
        value == "cn" ||
        value == "zh" ||
        value == "ch" ||
        value == "chi" ||
        value == "chinese" ||
        value.rfind("ch_", 0) == 0 ||
        value.rfind("zh-", 0) == 0 ||
        value.rfind("zh_", 0) == 0
    ) {
        return "ch";
    }
    return value;
}

std::string resolve_model_preference() {
    const char* raw = std::getenv(buzhidao_env::kFfiModelHint);
    return normalize_hint(raw == nullptr ? "" : raw);
}

std::vector<fs::path> list_named_submodel_dirs(const fs::path& model_root, const std::string& stem) {
    std::vector<fs::path> paths;
    if (model_root.empty()) {
        debug_log("list_named_submodel_dirs: empty model_root, stem=" + stem);
        return paths;
    }
    if (!directory_exists(model_root)) {
        debug_log("list_named_submodel_dirs: invalid model_root=" + model_root.string() + ", stem=" + stem);
        return paths;
    }
    const auto aliases = find_stem_aliases(stem);

    try {
        for (const auto& path : list_direct_child_dirs(model_root)) {
            const std::string name = to_lower_ascii(path.filename().string());
            const bool is_match = std::any_of(
                aliases.begin(),
                aliases.end(),
                [&](const auto& alias) { return name.find(alias) != std::string::npos; }
            );
            if (!is_match) {
                continue;
            }
            paths.push_back(path);
        }
    } catch (const std::exception& ex) {
        debug_log(
            std::string("list_named_submodel_dirs failed: ") + model_root.string() +
            ", stem=" + stem + ", error=" + ex.what()
        );
        return {};
    }

    std::sort(paths.begin(), paths.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename().string() < b.filename().string();
    });
    return paths;
}

std::string infer_model_family_hint(const fs::path& model_path, const std::string& stem) {
    auto name = to_lower_ascii(model_path.filename().string());
    const auto suffixes = find_stem_family_suffixes(stem);
    for (const auto& suffix : suffixes) {
        const auto pos = name.rfind(suffix);
        if (pos != std::string::npos) {
            auto family = name.substr(0, pos);
            while (!family.empty() && (family.back() == '_' || family.back() == '-')) {
                family.pop_back();
            }
            return family;
        }
    }
    return {};
}

bool prefer_model_by_lang(const std::string& candidate, const std::string& pref_source) {
    if (pref_source.empty()) {
        return false;
    }
    const auto lang_in = [&](std::initializer_list<const char*> tokens) {
        for (const auto* token : tokens) {
            if (pref_source == token) {
                return true;
            }
        }
        return false;
    };
    if (pref_source == "ch" || pref_source == "chinese_cht" || pref_source == "japan") {
        return candidate.find("server_rec") != std::string::npos;
    }
    if (pref_source == "en") {
        return candidate.find("en_") != std::string::npos;
    }
    if (pref_source == "korean") {
        return candidate.find("korean_") != std::string::npos;
    }
    if (pref_source == "th") {
        return candidate.find("th_") != std::string::npos;
    }
    if (pref_source == "el") {
        return candidate.find("el_") != std::string::npos;
    }
    if (pref_source == "te") {
        return candidate.find("te_") != std::string::npos;
    }
    if (pref_source == "ta") {
        return candidate.find("ta_") != std::string::npos;
    }
    if (lang_in({
            "af", "az", "bs", "cs", "cy", "da", "de", "es", "et", "fr", "ga", "hr", "hu", "id",
            "is", "it", "ku", "la", "lt", "lv", "mi", "ms", "mt", "nl", "no", "oc", "pi", "pl",
            "pt", "ro", "rs_latin", "sk", "sl", "sq", "sv", "sw", "tl", "tr", "uz", "vi",
            "french", "german", "fi", "eu", "gl", "lb", "rm", "ca", "qu"
        })) {
        return candidate.find("latin_") != std::string::npos;
    }
    if (pref_source == "ru" || pref_source == "be" || pref_source == "uk") {
        return candidate.find("eslav_") != std::string::npos;
    }
    if (lang_in({
            "rs_cyrillic", "bg", "mn", "abq", "ady", "kbd", "ava", "dar", "inh", "che", "lbe",
            "lez", "tab", "kk", "ky", "tg", "mk", "tt", "cv", "ba", "mhr", "mo", "udm", "kv",
            "os", "bua", "xal", "tyv", "sah", "kaa"
        })) {
        return candidate.find("cyrillic_") != std::string::npos;
    }
    if (lang_in({"ar", "fa", "ug", "ur", "ps", "sd", "bal"})) {
        return candidate.find("arabic_") != std::string::npos;
    }
    if (lang_in({"hi", "mr", "ne", "bh", "mai", "ang", "bho", "mah", "sck", "new", "gom", "sa", "bgc"})) {
        return candidate.find("devanagari_") != std::string::npos;
    }
    if (candidate == pref_source) {
        return true;
    }
    const std::string tokens[] = {
        "_" + pref_source + "_",
        "-" + pref_source + "-",
        "_" + pref_source,
        "-" + pref_source,
        pref_source + "_",
        pref_source + "-",
    };
    for (const auto& token : tokens) {
        if (candidate.find(token) != std::string::npos) {
            return true;
        }
    }
    return candidate.find(pref_source) != std::string::npos;
}

bool is_textline_orientation_model(const std::string& name) {
    return name.find("textline") != std::string::npos || name.find("text_line") != std::string::npos;
}

std::pair<fs::path, fs::path> resolve_candidate_model_pair(
    const fs::path& model_dir,
    const std::string& stem,
    const std::string& preferred_token,
    const std::string& preferred_lang,
    const std::string& preferred_family
) {
    debug_log(std::string("resolve_candidate_model_pair stem=") + stem +
              ", preferred_token=" + preferred_token +
              ", preferred_lang=" + preferred_lang +
              ", preferred_family=" + preferred_family +
              ", root=" + model_dir.string());
    const auto direct_json = model_dir / (stem + ".json");
    const auto direct_pdiparams = model_dir / (stem + ".pdiparams");
    const auto direct_pdmodel = model_dir / (stem + ".pdmodel");

    if (file_exists(direct_json) && file_exists(direct_pdiparams)) {
        return {direct_json, direct_pdiparams};
    }
    if (file_exists(direct_pdmodel) && file_exists(direct_pdiparams)) {
        return {direct_pdmodel, direct_pdiparams};
    }

    const fs::path direct_dir = model_dir / stem;
    std::vector<fs::path> candidates;
    if (directory_exists(direct_dir) && has_stem_files_in_dir(direct_dir)) {
        candidates.push_back(direct_dir);
    }

    auto named_dirs = list_named_submodel_dirs(model_dir, stem);
    candidates.insert(candidates.end(), named_dirs.begin(), named_dirs.end());

    for (const auto& path : candidates) {
        if (!has_stem_files_in_dir(path)) {
            continue;
        }
        const auto name = to_lower_ascii(path.filename().string());
        if (!preferred_token.empty() && name.find(preferred_token) != std::string::npos) {
            debug_log("candidate matched token: " + name);
            const auto preferred_json = path / "inference.json";
            const auto preferred_params = path / "inference.pdiparams";
            const auto preferred_model = path / "inference.pdmodel";
            if (file_exists(preferred_json) && file_exists(preferred_params)) {
                debug_log("candidate selected by token: " + name + ", " + preferred_json.string());
                return {preferred_json, preferred_params};
            }
            if (file_exists(preferred_model) && file_exists(preferred_params)) {
                debug_log("candidate selected by token: " + name + ", " + preferred_model.string());
                return {preferred_model, preferred_params};
            }
        }
    }

    if (!preferred_family.empty()) {
        for (const auto& path : candidates) {
            if (!has_stem_files_in_dir(path)) {
                continue;
            }
            const auto name = to_lower_ascii(path.filename().string());
            if (name.find(preferred_family) != std::string::npos) {
                const auto family_json = path / "inference.json";
                const auto family_params = path / "inference.pdiparams";
                const auto family_model = path / "inference.pdmodel";
                if (file_exists(family_json) && file_exists(family_params)) {
                    debug_log("candidate selected by family: " + name + ", " + family_json.string());
                    return {family_json, family_params};
                }
                if (file_exists(family_model) && file_exists(family_params)) {
                    debug_log("candidate selected by family: " + name + ", " + family_model.string());
                    return {family_model, family_params};
                }
            }
        }
    }

    for (const auto& path : candidates) {
        if (!has_stem_files_in_dir(path)) {
            continue;
        }
        const auto name = to_lower_ascii(path.filename().string());
        if (prefer_model_by_lang(name, preferred_lang)) {
            const auto model_json = path / "inference.json";
            const auto model_params = path / "inference.pdiparams";
            const auto model_model = path / "inference.pdmodel";
            if (file_exists(model_json) && file_exists(model_params)) {
                debug_log("candidate selected by lang: " + name + ", " + model_json.string());
                return {model_json, model_params};
            }
            if (file_exists(model_model) && file_exists(model_params)) {
                debug_log("candidate selected by lang: " + name + ", " + model_model.string());
                return {model_model, model_params};
            }
        }
    }

    if (stem == "cls") {
        for (const auto& path : candidates) {
            if (!has_stem_files_in_dir(path)) {
                continue;
            }
            const auto name = to_lower_ascii(path.filename().string());
            if (!is_textline_orientation_model(name)) {
                continue;
            }
            const auto preferred_json = path / "inference.json";
            const auto preferred_params = path / "inference.pdiparams";
            const auto preferred_model = path / "inference.pdmodel";
            if (file_exists(preferred_json) && file_exists(preferred_params)) {
                debug_log("candidate selected by cls textline preference: " + name + ", " + preferred_json.string());
                return {preferred_json, preferred_params};
            }
            if (file_exists(preferred_model) && file_exists(preferred_params)) {
                debug_log("candidate selected by cls textline preference: " + name + ", " + preferred_model.string());
                return {preferred_model, preferred_params};
            }
        }
    }

    for (const auto& path : candidates) {
        if (!has_stem_files_in_dir(path)) {
            continue;
        }
        const auto name = to_lower_ascii(path.filename().string());
        const auto model_json = path / "inference.json";
        const auto model_params = path / "inference.pdiparams";
        const auto model_model = path / "inference.pdmodel";
        if (file_exists(model_json) && file_exists(model_params)) {
            debug_log("candidate fallback: " + name + ", " + model_json.string());
            return {model_json, model_params};
        }
        if (file_exists(model_model) && file_exists(model_params)) {
            debug_log("candidate fallback: " + name + ", " + model_model.string());
            return {model_model, model_params};
        }
    }
    debug_log("candidate not found: " + stem + ", root=" + model_dir.string());
    return {"", ""};
}

fs::path find_named_submodel_dir(const fs::path& model_root, const std::string& stem) {
    const auto candidates = list_named_submodel_dirs(model_root, stem);
    if (candidates.empty()) {
        return {};
    }
    return candidates.front();
}

std::pair<fs::path, fs::path> resolve_model_pair(
    const fs::path& model_root,
    const std::string& stem,
    const std::string& preferred_model_hint,
    const std::string& preferred_lang,
    const std::string& preferred_family
) {
    debug_log(
        std::string("resolve_model_pair begin: root=") + model_root.string() +
        ", stem=" + stem +
        ", preferred_model_hint=" + preferred_model_hint +
        ", preferred_lang=" + preferred_lang +
        ", preferred_family=" + preferred_family
    );
    if (model_root.empty()) {
        debug_log("resolve_model_pair: empty model_root");
        return {};
    }
    const auto resolved_token = preferred_model_hint;
    const auto resolved_lang_value = preferred_lang.empty() ? std::string("en") : preferred_lang;
    debug_log(
        std::string("resolve_model_pair args resolved: stem=") + stem +
        ", token=" + resolved_token +
        ", lang=" + resolved_lang_value
    );
    debug_log(std::string("resolve_model_pair checking root: ") + model_root.string());
    if (!directory_exists(model_root)) {
        debug_log(std::string("resolve_model_pair: model_root is not a directory: ") + model_root.string());
        return {};
    }
    debug_log(std::string("resolve_model_pair root ok: ") + model_root.string());
    try {
        debug_log(std::string("resolve_model_pair candidate search begin: ") + stem);
        const auto result = resolve_candidate_model_pair(
            model_root,
            stem,
            resolved_token,
            resolved_lang_value,
            preferred_family
        );
        debug_log(
            std::string("resolve_model_pair done: ") + stem +
            ", json=" + result.first.string() +
            ", params=" + result.second.string()
        );
        return result;
    } catch (const std::exception& ex) {
        debug_log(std::string("resolve_model_pair exception: ") + stem + ", error=" + ex.what());
        return {};
    }
}
