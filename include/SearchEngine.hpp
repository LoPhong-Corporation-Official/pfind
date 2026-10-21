#ifndef PFIND_SEARCH_ENGINE_HPP
#define PFIND_SEARCH_ENGINE_HPP

#include "FileInfo.hpp"
#include "SearchOptions.hpp"
#include "Wildcard.hpp"

#include <functional>
#include <string>
#include <vector>

namespace pfind {

using SearchCallback = std::function<void(const FileInfo&)>;

class SearchEngine {
public:
    explicit SearchEngine(SearchOptions options);

    // Walks the tree and invokes the callback for every matching entry.
    // Returns the number of matches. Problems (permissions, broken links,
    // symlink loops, ...) are collected in errors() and never abort the walk.
    std::size_t search(const SearchCallback& callback) const;

    // True if the root itself could not be searched.
    bool rootFailed() const noexcept { return rootFailed_; }

    const std::vector<std::string>& errors() const noexcept;

private:
    SearchOptions options_;
    std::vector<Matcher> includes_;
    std::vector<Matcher> excludes_;

    mutable std::vector<std::string> errors_;
    mutable bool rootFailed_{false};

    bool matchesInclude(const std::string& name, const std::string& relativePath) const;
    bool isExcluded(const std::string& name) const;
    void addError(const std::string& message) const;
};

} // namespace pfind

#endif
