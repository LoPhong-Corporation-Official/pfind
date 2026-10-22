#include "SearchEngine.hpp"

#include "Utf8.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
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

// Ignore rules of one directory, chained to those of its parent directories.
struct IgnoreNode {
    std::string base;   // directory of the ignore file, '/' separators, no trailing '/'
    IgnoreRules rules;
    std::shared_ptr<const IgnoreNode> parent;
};

struct PendingDirectory {
    fs::path path;
    unsigned depth;  // depth of the entries inside this directory (1-based)
    std::shared_ptr<const AncestorNode> ancestors;
    std::shared_ptr<const IgnoreNode> ignores;   // rules of the parent directories
};

std::string directoryKey(const fs::path& dir) {
    std::string key = utf8::fromPathGeneric(dir);
    while (key.size() > 1 && key.back() == '/') {
        key.pop_back();
    }
    if (key == "/") {
        key.clear();   // so that base + "/" + name works for the file system root too
    }
    return key;
}

bool readTextFile(const fs::path& path, std::string& content) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// Reads the ignore files `names` found in `dir` and chains them in front of `parent`.
std::shared_ptr<const IgnoreNode> loadIgnoreNodes(const fs::path& dir,
                                                  const std::vector<std::string>& names,
                                                  std::shared_ptr<const IgnoreNode> parent) {
    for (const std::string& name : names) {
        std::string content;
        if (!readTextFile(dir / utf8::toPath(name), content)) {
            continue;
        }
        IgnoreRules rules = IgnoreRules::parse(content);
        if (rules.empty()) {
            continue;
        }
        parent = std::make_shared<IgnoreNode>(IgnoreNode{directoryKey(dir), std::move(rules), parent});
    }
    return parent;
}

// True if an enclosing ignore file ignores the entry (innermost file wins, last rule wins).
bool isIgnored(const std::shared_ptr<const IgnoreNode>& chain,
               const std::string& entryPath,
               const std::string& name,
               bool isDirectory) {
    for (const IgnoreNode* node = chain.get(); node != nullptr; node = node->parent.get()) {
        const std::string& base = node->base;
        if (entryPath.size() <= base.size() + 1 || entryPath.compare(0, base.size(), base) != 0 ||
            entryPath[base.size()] != '/') {
            continue;
        }

        const std::string_view relative = std::string_view(entryPath).substr(base.size() + 1);
        const auto verdict = node->rules.check(relative, name, isDirectory);
        if (verdict != IgnoreRules::Verdict::None) {
            return verdict == IgnoreRules::Verdict::Ignored;
        }
    }
    return false;
}

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
    bool highlight;
};

// How many bytes of a line are kept (never cutting inside a UTF-8 sequence).
std::size_t shownLength(std::string_view line) {
    constexpr std::size_t kMaxShownBytes = 300;
    if (line.size() <= kMaxShownBytes) {
        return line.size();
    }

    std::size_t cut = kMaxShownBytes;
    while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return cut;
}

ContentMatch makeLine(std::size_t number, std::string_view line, bool context, const Matcher* highlighter) {
    const std::size_t shown = shownLength(line);

    ContentMatch result;
    result.line = number;
    result.context = context;
    result.text = std::string(line.substr(0, shown));
    if (shown < line.size()) {
        result.text += "...";
    }
    if (highlighter != nullptr) {
        result.ranges = highlighter->findAll(std::string_view(result.text).substr(0, shown));
    }
    return result;
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
                out.push_back(makeLine(lineNumber, line, true, nullptr));
                --afterRemaining;
                continue;
            }

            if (matcher.matches(line)) {
                for (auto& previous : beforeBuffer) {
                    out.push_back(std::move(previous));
                }
                beforeBuffer.clear();

                out.push_back(makeLine(lineNumber, line, false, settings.highlight ? &matcher : nullptr));
                ++matches;
                afterRemaining = settings.after;

                if (matches >= settings.maxMatches) {
                    limitReached = true;
                    if (afterRemaining == 0) {
                        break;
                    }
                }
            } else if (afterRemaining > 0) {
                out.push_back(makeLine(lineNumber, line, true, nullptr));
                --afterRemaining;
            } else if (settings.before > 0) {
                beforeBuffer.push_back(makeLine(lineNumber, line, true, nullptr));
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

    // Use a normalised root ("a/../b/" -> "b") so ignore-file prefixes line up with entry paths.
    fs::path root = options_.root.lexically_normal();
    if (!root.has_filename() && root.has_parent_path()) {
        root = root.parent_path();
    }

    // ---- ignore files ----
    std::vector<std::string> ignoreNames;
    if (options_.useGitignore) {
        ignoreNames.push_back(".gitignore");
    }
    ignoreNames.insert(ignoreNames.end(), options_.ignoreFiles.begin(), options_.ignoreFiles.end());
    const bool ignoreActive = !ignoreNames.empty();

    std::shared_ptr<const IgnoreNode> rootIgnores;
    if (options_.useGitignore) {
        // Find the enclosing git repository, if any.
        fs::path repo;
        for (fs::path dir = root;;) {
            std::error_code gitEc;
            if (fs::exists(dir / ".git", gitEc)) {
                repo = dir;
                break;
            }
            fs::path parent = dir.parent_path();
            if (parent.empty() || parent == dir) {
                break;
            }
            dir = parent;
        }

        if (!repo.empty()) {
            // Lowest precedence first: info/exclude, then .gitignore from the repo root down to
            // the directory above the search root (the root's own file is read during the walk).
            rootIgnores = loadIgnoreNodes(repo / ".git" / "info", {"exclude"}, nullptr);
            if (rootIgnores) {
                // loadIgnoreNodes() based the node on the "info" directory; patterns in
                // info/exclude are relative to the repository root.
                auto fixed = std::make_shared<IgnoreNode>(
                    IgnoreNode{directoryKey(repo), rootIgnores->rules, nullptr});
                rootIgnores = fixed;
            }

            std::vector<fs::path> above;
            for (fs::path dir = root.parent_path(); !dir.empty(); dir = dir.parent_path()) {
                if (root == repo) {
                    break;
                }
                above.push_back(dir);
                if (dir == repo || dir == dir.parent_path()) {
                    break;
                }
            }
            for (auto it = above.rbegin(); it != above.rend(); ++it) {
                rootIgnores = loadIgnoreNodes(*it, {".gitignore"}, rootIgnores);
            }
        }
    }

    std::shared_ptr<const AncestorNode> rootAncestors;
    if (options_.followSymlinks) {
        const fs::path canonicalRoot = fs::canonical(root, ec);
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
        options_.highlightMatches && !options_.filesWithMatches,
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
    stack.push_back({root, 1, rootAncestors, rootIgnores});

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

        // Rules of this directory apply to its entries and to everything below.
        const std::shared_ptr<const IgnoreNode> chain =
            ignoreActive ? loadIgnoreNodes(current.path, ignoreNames, current.ignores) : nullptr;

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

            if (!skip && options_.skipHidden && !name.empty() && name[0] == '.') {
                skip = true;
            }

            if (!skip && options_.useGitignore && isDirectory && name == ".git") {
                skip = true;   // never descend into the repository metadata
            }

            if (!skip && chain && isIgnored(chain, utf8::fromPathGeneric(entryPath), name, isDirectory)) {
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
                        children.push_back({entryPath, current.depth + 1, childAncestors, chain});
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
                    relative = utf8::fromPathGeneric(entryPath.lexically_relative(root));
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
