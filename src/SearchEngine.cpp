#include "SearchEngine.hpp"

#include "Utf8.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

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

struct ScanSettings {
    std::size_t maxMatches;
    std::size_t before;
    std::size_t after;
};

std::string shorten(std::string_view line) {
    constexpr std::size_t kMaxShownBytes = 300;
    if (line.size() <= kMaxShownBytes) {
        return std::string(line);
    }

    std::size_t cut = kMaxShownBytes;
    // Do not cut inside a UTF-8 sequence.
    while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return std::string(line.substr(0, cut)) + "...";
}

// Reads lines from a stream in large chunks. Lines are returned as views into the internal buffer
// (valid until the next call), which avoids a copy per line.
class LineReader {
public:
    explicit LineReader(std::ifstream& in) : in_(in), buffer_(threadBuffer()) {
        refill();
    }

    // The first bytes of the file, for binary detection.
    std::string_view head(std::size_t count) const {
        return std::string_view(buffer_.data(), std::min(count, end_));
    }

    // False at end of input, or when a line exceeds the size limit (see tooLong()).
    bool next(std::string_view& out) {
        carry_.clear();
        bool haveCarry = false;

        for (;;) {
            if (pos_ == end_ && !refill()) {
                if (haveCarry) {
                    out = carry_;
                    return true;
                }
                return false;
            }

            const char* base = buffer_.data() + pos_;
            const std::size_t available = end_ - pos_;
            const void* newline = std::memchr(base, '\n', available);

            if (newline != nullptr) {
                const auto length = static_cast<std::size_t>(static_cast<const char*>(newline) - base);
                pos_ += length + 1;
                if (!haveCarry) {
                    out = std::string_view(base, length);
                } else {
                    carry_.append(base, length);
                    out = carry_;
                }
                return true;
            }

            // Line continues in the next chunk.
            carry_.append(base, available);
            haveCarry = true;
            pos_ = end_;
            if (carry_.size() > kMaxLineBytes) {
                tooLong_ = true;
                return false;
            }
        }
    }

    bool tooLong() const noexcept { return tooLong_; }

    static constexpr std::size_t kMaxLineBytes = 1 << 20;   // longer lines are treated as binary data

private:
    static std::vector<char>& threadBuffer() {
        thread_local std::vector<char> buffer(256 * 1024);
        return buffer;
    }

    bool refill() {
        in_.read(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
        end_ = static_cast<std::size_t>(in_.gcount());
        pos_ = 0;
        return end_ > 0;
    }

    std::ifstream& in_;
    std::vector<char>& buffer_;
    std::string carry_;
    std::size_t pos_{0};
    std::size_t end_{0};
    bool tooLong_{false};
};

// Scans a text file line by line. Returns false (and no matches) for binary or unreadable files.
// Context lines are added to `out` flagged with context = true; they never count as matches.
bool scanFile(const fs::path& path,
              const Matcher& matcher,
              const ScanSettings& settings,
              std::vector<ContentMatch>& out,
              std::string& error) {
    constexpr std::size_t kProbeSize = 8192;

    try {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error = "Cannot open file";
            return false;
        }

        LineReader reader(in);

        // Binary detection: a NUL byte near the start.
        if (reader.head(kProbeSize).find('\0') != std::string_view::npos) {
            return false;
        }

        std::deque<ContentMatch> beforeBuffer;
        std::size_t matches = 0;
        std::size_t afterRemaining = 0;
        bool limitReached = false;
        std::size_t lineNumber = 0;
        std::string_view line;

        while (reader.next(line)) {
            ++lineNumber;
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }

            if (limitReached) {
                // Only trailing context is still wanted.
                if (afterRemaining == 0) {
                    break;
                }
                out.push_back({lineNumber, shorten(line), true});
                --afterRemaining;
                continue;
            }

            if (matcher.matches(line)) {
                for (auto& previous : beforeBuffer) {
                    out.push_back(std::move(previous));
                }
                beforeBuffer.clear();

                out.push_back({lineNumber, shorten(line), false});
                ++matches;
                afterRemaining = settings.after;

                if (matches >= settings.maxMatches) {
                    limitReached = true;
                    if (afterRemaining == 0) {
                        break;
                    }
                }
            } else if (afterRemaining > 0) {
                out.push_back({lineNumber, shorten(line), true});
                --afterRemaining;
            } else if (settings.before > 0) {
                beforeBuffer.push_back({lineNumber, shorten(line), true});
                if (beforeBuffer.size() > settings.before) {
                    beforeBuffer.pop_front();
                }
            }
        }

        return matches > 0;
    } catch (const std::exception& exception) {
        // e.g. std::regex running out of resources on pathological input.
        out.clear();
        error = std::string("Content scan failed (") + exception.what() + ")";
        return false;
    }
}

struct PendingScan {
    FileInfo info;
    fs::path path;
};

struct ScanResult {
    bool found{false};
    std::vector<ContentMatch> matches;
    std::string error;
};

} // namespace

SearchEngine::SearchEngine(SearchOptions options)
    : options_(std::move(options)) {
    MatchOptions matchOptions;
    matchOptions.caseSensitive = options_.caseSensitive;
    matchOptions.ignoreAccents = options_.ignoreAccents;

    MatchOptions nameOptions = matchOptions;
    nameOptions.regex = options_.regex;

    for (const auto& pattern : options_.patterns) {
        includes_.emplace_back(pattern, nameOptions);
    }
    for (const auto& pattern : options_.excludes) {
        excludes_.emplace_back(pattern, nameOptions);
    }

    if (options_.contentPattern) {
        MatchOptions contentOptions = matchOptions;
        contentOptions.regex = options_.contentRegex;
        contentMatcher_ = options_.contentRegex ? Matcher(*options_.contentPattern, contentOptions)
                                                : Matcher::literal(*options_.contentPattern, contentOptions);
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

    // ---- content scanning (optionally parallel, results always in traversal order) ----
    constexpr std::size_t kBatchSize = 128;

    ScanSettings scanSettings{
        options_.filesWithMatches ? 1 : options_.maxLinesPerFile.value_or(static_cast<std::size_t>(-1)),
        options_.filesWithMatches ? 0 : options_.beforeContext,
        options_.filesWithMatches ? 0 : options_.afterContext,
    };

    unsigned jobs = 1;
    if (contentMatcher_) {
        jobs = options_.jobs.value_or(std::clamp(std::thread::hardware_concurrency(), 1u, 8u));
        jobs = std::max(jobs, 1u);
    }

    std::vector<PendingScan> batch;

    // Scans the queued files and reports the hits in order. Returns true once maxResults is reached.
    auto flush = [&]() -> bool {
        if (batch.empty()) {
            return false;
        }

        std::vector<ScanResult> results(batch.size());
        std::atomic<std::size_t> next{0};

        auto worker = [&]() {
            for (;;) {
                const std::size_t i = next.fetch_add(1);
                if (i >= batch.size()) {
                    break;
                }
                results[i].found = scanFile(batch[i].path, *contentMatcher_, scanSettings,
                                            results[i].matches, results[i].error);
            }
        };

        const std::size_t threadCount = std::min<std::size_t>(jobs, batch.size());
        std::vector<std::thread> pool;
        for (std::size_t t = 1; t < threadCount; ++t) {
            pool.emplace_back(worker);
        }
        worker();
        for (auto& thread : pool) {
            thread.join();
        }

        bool done = false;
        for (std::size_t i = 0; i < batch.size() && !done; ++i) {
            if (!results[i].error.empty()) {
                addError(results[i].error + ": " + utf8::fromPath(batch[i].path));
            }
            if (results[i].found) {
                batch[i].info.matches = std::move(results[i].matches);
                callback(batch[i].info);
                ++count;
                done = options_.maxResults && count >= *options_.maxResults;
            }
        }

        batch.clear();
        return done;
    };

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

                    if (accepted && contentMatcher_) {
                        if (!isRegular) {
                            accepted = false;  // only regular files have content
                        } else {
                            batch.push_back({std::move(info), entryPath});
                            accepted = false;  // reported by flush(), in order
                            if (batch.size() >= kBatchSize && flush()) {
                                return count;
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

    flush();
    return count;
}

const std::vector<std::string>& SearchEngine::errors() const noexcept {
    return errors_;
}

} // namespace pfind
