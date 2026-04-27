#ifndef BUZHIDAO_PADDLE_BRIDGE_MODEL_H
#define BUZHIDAO_PADDLE_BRIDGE_MODEL_H

#include <filesystem>
#include <string>
#include <utility>

std::string resolve_preferred_lang();
std::string resolve_model_preference();
std::string infer_model_family_hint(
    const std::filesystem::path& model_path,
    const std::string& stem
);
std::filesystem::path find_named_submodel_dir(
    const std::filesystem::path& model_root,
    const std::string& stem
);
std::pair<std::filesystem::path, std::filesystem::path> resolve_model_pair(
    const std::filesystem::path& model_root,
    const std::string& stem,
    const std::string& preferred_model_hint,
    const std::string& preferred_lang,
    const std::string& preferred_family
);

#endif
