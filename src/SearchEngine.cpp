#include "SearchEngine.hpp"

#include "Utf8.hpp"

#include <memory>
#include <system_error>

namespace pfind {
namespace fs = std::filesystem;

namespace {

// Chain of canonical ancestor directories, used only when following symlinks.
struct AncestorNode {
    fs::path canonical;
    std::shared_ptr<const AncestorNode> parent;
};

struct PendingDirectory {
    fs::path path;
    unsigned depth;  // depth of the entries inside this directory (1-based)
    std::shared_ptr<const AncestorNode> ancestors;
};

bool isAncestor(const std::shared_ptr<const AncestorNode>& chain, const fs::path& canonical) {
    for (const AncestorNode* node = chain.get(); node != nullptr; node = node->parent.get()) {
        if (node->canonical == canonical) {
            return true;
        }
    }
    return false;
}

} // namespace

SearchEngine::SearchEngine(SearchOptions options)
    : options_(std::move(options)) {
    MatchOptions matchOptions;
    matchOptions.caseSensitive = options_.caseSensitive;
    matchOptions.ignoreAccents = options_.ignoreAccents;

    for (const auto& pattern : options_.patterns) {
        includes_.emplace_back(pattern, matchOptions);
    }
    for (const auto& pattern : options_.excludes) {
        excludes_.emplace_back(pattern, matchOptions);
    }
}

bool SearchEngine::matchesInclude(const std::string& name, const std::string& relativePath) const {
    if (includes_.empty()) {
        return true;
    }

    const std::string& subject = options_.matchFullPath ? relativePath : name;
    for (const Matcher& matcher : includes_) {
        if (matcher.matches(subject)) {
            return true;
        }
    }
    return false;
}

bool SearchEngine::isExcluded(const std::string& name) const {
    for (const Matcher& matcher : excludes_) {
        if (matcher.matches(name)) {
            return true;
        }
    }
    return false;
}

void SearchEngine::addError(const std::string& message) const {
    errors_.push_back(message);
}

std::size_t SearchEngine::search(const SearchCallback& callback) const {
    errors_.clear();
    rootFailed_ = false;

    std::error_code ec;
    const auto rootStatus = fs::status(options_.root, ec);

    if (ec) {
        addError("Cannot access root: " + utf8::fromPath(options_.root) + " (" + ec.message() + ")");
        rootFailed_ = true;
        return 0;
    }

    if (!fs::is_directory(rootStatus)) {
        addError("Search root is not a directory: " + utf8::fromPath(options_.root));
        rootFailed_ = true;
        return 0;
    }

    std::shared_ptr<const AncestorNode> rootAncestors;
    if (options_.followSymlinks) {
        const fs::path canonicalRoot = fs::canonical(options_.root, ec);
        if (!ec) {
            rootAncestors = std::make_shared<AncestorNode>(AncestorNode{canonicalRoot, nullptr});
        }
        ec.clear();
    }

    const bool needMetadata = options_.minSize || options_.maxSize || options_.modifiedWithin ||
                              options_.showDetails || options_.collectMetadata;

    std::size_t count = 0;
    std::vector<PendingDirectory> stack;
    stack.push_back({options_.root, 1, rootAncestors});

    while (!stack.empty()) {
        PendingDirectory current = std::move(stack.back());
        stack.pop_back();

        fs::directory_iterator it(current.path, ec);
        if (ec) {
            addError("Cannot read directory: " + utf8::fromPath(current.path) +
                     " (" + ec.message() + ")");
            ec.clear();
            continue;
        }

        std::vector<PendingDirectory> children;
        const fs::directory_iterator end;

        while (it != end) {
            const fs::directory_entry entry = *it;
            const fs::path entryPath = entry.path();

            std::error_code symlinkEc;
            std::error_code dirEc;
            std::error_code fileEc;
            const bool isSymlink = entry.is_symlink(symlinkEc);
            const bool isDirectory = entry.is_directory(dirEc);
            const bool isRegular = entry.is_regular_file(fileEc);

            const std::error_code typeEc = dirEc ? dirEc : fileEc;
            bool skip = false;

            if (typeEc) {
                // Dangling symlinks are normal; anything else deserves a warning.
                if (!(isSymlink && !symlinkEc)) {
                    addError("Cannot inspect: " + utf8::fromPath(entryPath) +
                             " (" + typeEc.message() + ")");
                }
                skip = true;
            }

            const std::string name = utf8::fromPath(entryPath.filename());

            if (!skip && isExcluded(name)) {
                skip = true;
            }

            if (!skip) {
                // Descend into directories (even if the directory itself is not reported).
                const bool withinDepth = !options_.maxDepth || current.depth < *options_.maxDepth;
                if (isDirectory && withinDepth && (!isSymlink || options_.followSymlinks)) {
                    std::shared_ptr<const AncestorNode> childAncestors;
                    bool descend = true;

                    if (options_.followSymlinks) {
                        std::error_code canonicalEc;
                        const fs::path canonical = fs::canonical(entryPath, canonicalEc);
                        if (canonicalEc) {
                            addError("Cannot resolve: " + utf8::fromPath(entryPath) +
                                     " (" + canonicalEc.message() + ")");
                            descend = false;
                        } else if (isAncestor(current.ancestors, canonical)) {
                            addError("Symlink loop skipped: " + utf8::fromPath(entryPath));
                            descend = false;
                        } else {
                            childAncestors = std::make_shared<AncestorNode>(
                                AncestorNode{canonical, current.ancestors});
                        }
                    }

                    if (descend) {
                        children.push_back({entryPath, current.depth + 1, childAncestors});
                    }
                }

                bool typeOk = false;
                switch (options_.type) {
                    case EntryType::File:      typeOk = isRegular; break;
                    case EntryType::Directory: typeOk = isDirectory; break;
                    case EntryType::Any:       typeOk = isRegular || isDirectory; break;
                }

                std::string relative;
                if (typeOk && options_.matchFullPath) {
                    relative = utf8::fromPathGeneric(entryPath.lexically_relative(options_.root));
                }

                if (typeOk && matchesInclude(name, relative)) {
                    FileInfo info;
                    info.path = entryPath;
                    info.isDirectory = isDirectory && !isRegular;

                    bool accepted = true;

                    if (needMetadata) {
                        if (isRegular) {
                            std::error_code sizeEc;
                            const auto size = entry.file_size(sizeEc);
                            if (sizeEc) {
                                addError("Cannot read size: " + utf8::fromPath(entryPath) +
                                         " (" + sizeEc.message() + ")");
                                accepted = !(options_.minSize || options_.maxSize);
                            } else {
                                info.size = size;
                            }
                        } else if (options_.minSize || options_.maxSize) {
                            accepted = false;  // size filters only make sense for files
                        }

                        std::error_code timeEc;
                        const auto time = entry.last_write_time(timeEc);
                        if (timeEc) {
                            addError("Cannot read time: " + utf8::fromPath(entryPath) +
                                     " (" + timeEc.message() + ")");
                            if (options_.modifiedWithin) {
                                accepted = false;
                            }
                        } else {
                            info.lastWriteTime = time;
                        }

                        if (accepted && options_.minSize && info.size < *options_.minSize) {
                            accepted = false;
                        }
                        if (accepted && options_.maxSize && info.size > *options_.maxSize) {
                            accepted = false;
                        }
                        if (accepted && options_.modifiedWithin) {
                            const auto age = fs::file_time_type::clock::now() - info.lastWriteTime;
                            if (age > *options_.modifiedWithin) {
                                accepted = false;
                            }
                        }
                    }

                    if (accepted) {
                        callback(info);
                        ++count;

                        if (options_.maxResults && count >= *options_.maxResults) {
                            return count;
                        }
                    }
                }
            }

            it.increment(ec);
            if (ec) {
                addError("Directory traversal error in " + utf8::fromPath(current.path) +
                         " (" + ec.message() + ")");
                ec.clear();
                break;
            }
        }

        // Reverse so directories are visited in listing order.
        for (auto child = children.rbegin(); child != children.rend(); ++child) {
            stack.push_back(std::move(*child));
        }
    }

    return count;
}

const std::vector<std::string>& SearchEngine::errors() const noexcept {
    return errors_;
}

} // namespace pfind
