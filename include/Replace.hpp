#ifndef PFIND_REPLACE_HPP
#define PFIND_REPLACE_HPP

#include "Wildcard.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pfind {

struct LineChange {
    std::size_t line;       // 1-based
    std::string before;     // shortened for display
    std::string after;
};

struct ReplaceResult {
    std::vector<LineChange> changes;
    std::size_t replacements{0};
    bool written{false};
    std::string error;      // non-empty if the file was skipped or could not be written
};

// Replaces every hit of `matcher` in the text file at `path`, line by line.
//
// Line endings (LF / CRLF), a missing final newline and all other bytes are preserved. Binary files
// and files over 256 MB are skipped. With apply == false the file is only analysed. Otherwise the
// new content is written to a temporary file next to the original, which gets the original's
// permissions and then atomically replaces it (a symbolic link is resolved first, so the link is
// kept). With backup == true the original is first copied to "<file>.bak".
ReplaceResult replaceInFile(const std::filesystem::path& path,
                            const Matcher& matcher,
                            std::string_view replacement,
                            bool apply,
                            bool backup);

} // namespace pfind

#endif
