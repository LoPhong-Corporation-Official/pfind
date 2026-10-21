#ifndef PFIND_FILE_INFO_HPP
#define PFIND_FILE_INFO_HPP

#include <cstdint>
#include <filesystem>

namespace pfind {

struct FileInfo {
    std::filesystem::path path;
    bool isDirectory{false};
    std::uintmax_t size{0};
    std::filesystem::file_time_type lastWriteTime{};
};

} // namespace pfind

#endif
