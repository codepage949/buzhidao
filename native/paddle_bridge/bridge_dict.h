#ifndef BUZHIDAO_PADDLE_BRIDGE_DICT_H
#define BUZHIDAO_PADDLE_BRIDGE_DICT_H

#include <filesystem>
#include <string>
#include <vector>

std::vector<std::string> load_recognition_dict(
    const std::filesystem::path& model_dir,
    std::string* err
);
bool validate_recognition_dict(std::vector<std::string>* dict, std::string* err);

#endif
