#ifndef PFIND_SEARCH_OPTIONS_HPP
#define PFIND_SEARCH_OPTIONS_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pfind {

enum class EntryType { File, Directory, Any };

struct SearchOptions {
    std::filesystem::path root;

    // Entries match if ANY pattern matches. Empty means "match everything".
    std::vector<std::string> patterns;
    // Entries (and whole directories) whose name matches ANY of these are skipped.
    std::vector<std::string> excludes;

    EntryType type{EntryType::File};
    bool caseSensitive{false};
    bool ignoreAccents{false};
    bool matchFullPath{false};   // match against the path relative to root instead of the name
    bool followSymlinks{false};
    bool showDetails{false};
    bool collectMetadata{false}; // fill size/time even without filters (sorting, JSON output)

    std::optional<unsigned> maxDepth;                 // 1 = direct children of root
    std::optional<std::uintmax_t> minSize;            // bytes
    std::optional<std::uintmax_t> maxSize;            // bytes
    std::optional<std::chrono::seconds> modifiedWithin;
    std::optional<std::size_t> maxResults;
};

} // namespace pfind

#endif
