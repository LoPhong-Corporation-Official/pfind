#ifndef PFIND_FILE_INFO_HPP
#define PFIND_FILE_INFO_HPP

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace pfind {

struct ContentMatch {
    std::size_t line{0};   // 1-based
    std::string text;      // the line (truncated if very long)
    bool context{false};   // true for surrounding lines shown with -A/-B/-C
};

struct FileInfo {
    std::filesystem::path path;
    bool isDirectory{false};
    std::uintmax_t size{0};
    std::filesystem::file_time_type lastWriteTime{};
    std::vector<ContentMatch> matches;  // only filled for content searches
};

} // namespace pfind

#endif
