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
    bool regex{false};           // name patterns and excludes are regular expressions
    bool contentRegex{false};    // the --grep text is a regular expression
    bool caseSensitive{false};
    bool ignoreAccents{false};
    bool matchFullPath{false};   // match against the path relative to root instead of the name
    bool followSymlinks{false};
    bool useGitignore{false};            // honour .gitignore files (also parents inside a git repo)
    std::vector<std::string> ignoreFiles; // extra ignore-file names read in every directory
    bool skipHidden{false};              // skip names starting with '.'
    bool highlightMatches{false};        // record the matching ranges inside content-match lines
    bool showDetails{false};
    bool collectMetadata{false}; // fill size/time even without filters (sorting, JSON output)

    // Content search: only report files that contain this text (binary files are skipped).
    std::optional<std::string> contentPattern;
    bool filesWithMatches{false};                     // stop at the first matching line per file
    std::optional<std::size_t> maxLinesPerFile;
    std::size_t beforeContext{0};                     // lines shown before each match
    std::size_t afterContext{0};                      // lines shown after each match
    std::optional<unsigned> jobs;                     // content-scan threads (default: automatic)

    std::optional<unsigned> maxDepth;                 // 1 = direct children of root
    std::optional<std::uintmax_t> minSize;            // bytes
    std::optional<std::uintmax_t> maxSize;            // bytes
    std::optional<std::chrono::seconds> modifiedWithin;
    std::optional<std::size_t> maxResults;
};

} // namespace pfind

#endif
