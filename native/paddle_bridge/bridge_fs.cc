#include "bridge_fs.h"

#include "bridge_utils.h"

#include <fstream>
#include <string>

#if !defined(_WIN32)
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

bool file_exists(const fs::path& path) {
    if (path.empty()) {
        return false;
    }
#if defined(_WIN32)
    try {
        return fs::exists(path) && fs::is_regular_file(path);
    } catch (const std::exception& ex) {
        debug_log(std::string("file_exists failed: ") + path.string() + ", error=" + ex.what());
        return false;
    }
#else
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool directory_exists(const fs::path& path) {
    if (path.empty()) {
        return false;
    }
#if defined(_WIN32)
    try {
        return fs::exists(path) && fs::is_directory(path);
    } catch (const std::exception& ex) {
        debug_log(std::string("directory_exists failed: ") + path.string() + ", error=" + ex.what());
        return false;
    }
#else
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

std::vector<fs::path> list_direct_child_dirs(const fs::path& root) {
    std::vector<fs::path> dirs;
    if (root.empty()) {
        return dirs;
    }
#if defined(_WIN32)
    try {
        for (const auto& entry : fs::directory_iterator(root)) {
            if (entry.is_directory()) {
                dirs.push_back(entry.path());
            }
        }
    } catch (const std::exception& ex) {
        debug_log(std::string("list_direct_child_dirs failed: ") + root.string() + ", error=" + ex.what());
        return {};
    }
#else
    DIR* dir = ::opendir(root.c_str());
    if (dir == nullptr) {
        return dirs;
    }
    while (dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        const fs::path child = root / name;
        if (directory_exists(child)) {
            dirs.push_back(child);
        }
    }
    ::closedir(dir);
#endif
    return dirs;
}

std::vector<uint8_t> read_all_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }
    const auto size = in.tellg();
    if (size <= 0) {
        return {};
    }
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

bool has_stem_files_in_dir(const fs::path& dir) {
    if (dir.empty()) {
        return false;
    }
    const fs::path infer_json = dir / "inference.json";
    const fs::path infer_pdiparams = dir / "inference.pdiparams";
    const fs::path infer_pdmodel = dir / "inference.pdmodel";
    try {
        return (file_exists(infer_json) && file_exists(infer_pdiparams)) ||
            (file_exists(infer_pdmodel) && file_exists(infer_pdiparams));
    } catch (const std::exception& ex) {
        debug_log(std::string("has_stem_files_in_dir failed: ") + dir.string() + ", error=" + ex.what());
        return false;
    }
}
