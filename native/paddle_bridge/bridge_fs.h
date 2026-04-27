#ifndef BUZHIDAO_PADDLE_BRIDGE_FS_H
#define BUZHIDAO_PADDLE_BRIDGE_FS_H

#include <cstdint>
#include <filesystem>
#include <vector>

bool file_exists(const std::filesystem::path& path);
bool directory_exists(const std::filesystem::path& path);
std::vector<std::filesystem::path> list_direct_child_dirs(const std::filesystem::path& root);
std::vector<uint8_t> read_all_bytes(const std::filesystem::path& path);
bool has_stem_files_in_dir(const std::filesystem::path& dir);

#endif
