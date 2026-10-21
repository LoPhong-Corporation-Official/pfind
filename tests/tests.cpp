// Minimal dependency-free test runner.
#include "CLIParser.hpp"
#include "Format.hpp"
#include "SearchEngine.hpp"
#include "Utf8.hpp"
#include "Wildcard.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

namespace fs = std::filesystem;

namespace {

int failures = 0;
int checks = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(expr)) {                                                           \
            ++failures;                                                          \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #expr "\n"; \
        }                                                                        \
    } while (false)

bool match(const char* text, const char* pattern, bool caseSensitive = false, bool accents = false) {
    pfind::MatchOptions options;
    options.caseSensitive = caseSensitive;
    options.ignoreAccents = accents;
    return pfind::Matcher(pattern, options).matches(text);
}

void testWildcard() {
    CHECK(match("main.cpp", "*.cpp"));
    CHECK(match("main.cpp", "main.???"));
    CHECK(!match("main.cpp", "*.hpp"));
    CHECK(match("", "*"));
    CHECK(match("abc", "a*b*c"));
    CHECK(!match("abc", "a*b*d"));
    CHECK(match("aaaaaaaaaaaaaaaaaaaaaaaab", "*a*a*a*a*a*b"));  // no exponential blow-up

    // Case handling.
    CHECK(match("README.MD", "readme.md"));
    CHECK(!match("README.MD", "readme.md", true));

    // Character classes and escapes.
    CHECK(match("file1.txt", "file[0-9].txt"));
    CHECK(!match("fileA.txt", "file[0-9].txt"));
    CHECK(match("fileA.txt", "file[!0-9].txt"));
    CHECK(match("a*b", "a\\*b"));
    CHECK(!match("axb", "a\\*b"));
    CHECK(match("a[b", "a[b"));  // unterminated class is a literal

    // Unicode: '?' is one character, not one byte; case folding works for Vietnamese.
    CHECK(match("tệp", "t?p"));
    CHECK(match("TÀI LIỆU.txt", "tài liệu.*"));
    CHECK(match("Đường", "đường"));
    CHECK(!match("Tài liệu", "tai lieu*"));
    CHECK(match("Tài liệu", "tai lieu*", false, true));
    CHECK(match("Đà Nẵng", "da nang", false, true));

    // Decomposed (NFD) text, e.g. from macOS file systems.
    CHECK(match("Ta\xCC\x80i li\xC3\xAA\xCC\xA3u", "tai lieu", false, true));

    // Invalid UTF-8 must not crash.
    CHECK(match("\xFF\xFE.bin", "*.bin"));

    // Legacy API.
    CHECK(pfind::wildcardMatch("Test.TXT", "*.txt"));
    CHECK(!pfind::wildcardMatch("Test.TXT", "*.txt", true));
}

void testUtf8() {
    const std::string text = "Việt Nam \xF0\x9F\x98\x80";
    CHECK(pfind::utf8::encode(pfind::utf8::decode(text)) == text);

    const std::string broken = "a\xFF" "b\xC3";
    CHECK(pfind::utf8::encode(pfind::utf8::decode(broken)) == broken);

    CHECK(pfind::utf8::foldCase(U'Ế') == U'ế');
    CHECK(pfind::utf8::stripAccent(U'ế') == U'e');
    CHECK(pfind::utf8::stripAccent(U'đ') == U'd');
}

void testFormat() {
    CHECK(pfind::parseSize("10") == 10);
    CHECK(pfind::parseSize("10k") == 10240);
    CHECK(pfind::parseSize("5MB") == 5ULL * 1024 * 1024);
    CHECK(pfind::parseSize("2g") == 2ULL * 1024 * 1024 * 1024);
    CHECK(!pfind::parseSize("abc"));
    CHECK(!pfind::parseSize("10x"));
    CHECK(!pfind::parseSize("99999999999999999999T"));
    CHECK(!pfind::parseSize(""));

    CHECK(pfind::parseDuration("90")->count() == 90);
    CHECK(pfind::parseDuration("2h")->count() == 7200);
    CHECK(pfind::parseDuration("1w")->count() == 604800);
    CHECK(!pfind::parseDuration("5y"));

    CHECK(pfind::parseUnsigned("42") == 42);
    CHECK(!pfind::parseUnsigned("-1"));
    CHECK(!pfind::parseUnsigned("4 2"));

    CHECK(pfind::formatSize(512) == "512 B");
    CHECK(pfind::formatSize(1536) == "1.50 KB");
    CHECK(pfind::jsonEscape("a\"b\\c\n") == "a\\\"b\\\\c\\n");
}

void testCli() {
    using pfind::CLIParser;

    auto ok = CLIParser::parse({"pfind", ".", "*.cpp", "*.hpp", "-x", "build", "--max-depth=3", "--min-size", "1k"});
    CHECK(ok.valid);
    CHECK(ok.options.patterns.size() == 2);
    CHECK(ok.options.excludes.size() == 1);
    CHECK(ok.options.maxDepth == 3u);
    CHECK(ok.options.minSize == 1024u);

    CHECK(CLIParser::parse({"pfind"}).showHelp);
    CHECK(CLIParser::parse({"pfind", "-h"}).showHelp);
    CHECK(CLIParser::parse({"pfind", "-v"}).showVersion);

    CHECK(!CLIParser::parse({"pfind", ".", "--bogus"}).valid);
    CHECK(!CLIParser::parse({"pfind", ".", "--pattern"}).valid);
    CHECK(!CLIParser::parse({"pfind", ".", "--max-depth", "0"}).valid);
    CHECK(!CLIParser::parse({"pfind", ".", "--min-size", "5k", "--max-size", "1k"}).valid);
    CHECK(!CLIParser::parse({"pfind", "--case-sensitive"}).valid);  // missing path
    CHECK(!CLIParser::parse({"pfind", ".", "--json", "--print0"}).valid);

    // "--" lets a pattern start with a dash.
    auto dash = CLIParser::parse({"pfind", ".", "--", "-weird*"});
    CHECK(dash.valid && dash.options.patterns.size() == 1);
}

void touch(const fs::path& path, const std::string& content = "x") {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
}

std::set<std::string> names(const pfind::SearchOptions& options, std::size_t* count = nullptr) {
    pfind::SearchEngine engine(options);
    std::set<std::string> found;
    const std::size_t n = engine.search([&](const pfind::FileInfo& info) {
        found.insert(pfind::utf8::fromPath(info.path.filename()));
    });
    if (count) *count = n;
    return found;
}

void testEngine() {
    const fs::path root = fs::temp_directory_path() / "pfind_test_tree";
    fs::remove_all(root);

    touch(root / "a.cpp");
    touch(root / "b.hpp");
    touch(root / "sub" / "c.cpp", std::string(2048, 'x'));
    touch(root / "sub" / "deep" / "d.cpp");
    touch(root / "node_modules" / "skipped.cpp");
    touch(root / pfind::utf8::toPath("Tài liệu") / pfind::utf8::toPath("Báo cáo.txt"));

    pfind::SearchOptions base;
    base.root = root;

    {
        auto options = base;
        options.patterns = {"*.cpp"};
        CHECK((names(options) == std::set<std::string>{"a.cpp", "c.cpp", "d.cpp", "skipped.cpp"}));
    }
    {
        auto options = base;
        options.patterns = {"*.cpp", "*.hpp"};
        options.excludes = {"node_modules"};
        CHECK((names(options) == std::set<std::string>{"a.cpp", "b.hpp", "c.cpp", "d.cpp"}));
    }
    {
        auto options = base;
        options.patterns = {"*.cpp"};
        options.maxDepth = 1;
        CHECK((names(options) == std::set<std::string>{"a.cpp"}));
        options.maxDepth = 2;
        CHECK((names(options) == std::set<std::string>{"a.cpp", "c.cpp", "skipped.cpp"}));
    }
    {
        auto options = base;
        options.minSize = 1024;
        CHECK((names(options) == std::set<std::string>{"c.cpp"}));
    }
    {
        auto options = base;
        options.type = pfind::EntryType::Directory;
        CHECK(names(options).count("sub") == 1);
        CHECK(names(options).count("a.cpp") == 0);
    }
    {
        auto options = base;
        options.patterns = {"bao cao*"};
        options.ignoreAccents = true;
        CHECK((names(options) == std::set<std::string>{"Báo cáo.txt"}));
    }
    {
        auto options = base;
        options.patterns = {"sub/*.cpp"};
        options.matchFullPath = true;
        CHECK((names(options) == std::set<std::string>{"c.cpp", "d.cpp"}));
    }
    {
        auto options = base;
        options.patterns = {"*.cpp"};
        options.maxResults = 2;
        std::size_t count = 0;
        names(options, &count);
        CHECK(count == 2);
    }
    {
        auto options = base;
        options.modifiedWithin = std::chrono::hours(1);
        CHECK(names(options).count("a.cpp") == 1);
    }
    {
        auto options = base;
        options.root = root / "does-not-exist";
        pfind::SearchEngine engine(options);
        CHECK(engine.search([](const pfind::FileInfo&) {}) == 0);
        CHECK(engine.rootFailed());
        CHECK(!engine.errors().empty());
    }

#ifndef _WIN32
    {
        // Symlink loop must terminate and produce a warning, not hang.
        std::error_code ec;
        fs::create_directory_symlink(root, root / "sub" / "loop", ec);
        if (!ec) {
            auto options = base;
            options.patterns = {"a.cpp"};
            options.followSymlinks = true;
            pfind::SearchEngine engine(options);
            std::size_t found = engine.search([](const pfind::FileInfo&) {});
            CHECK(found >= 1);
            CHECK(!engine.errors().empty());
        }
    }
#endif

    fs::remove_all(root);
}

} // namespace

int main() {
    testWildcard();
    testUtf8();
    testFormat();
    testCli();
    testEngine();

    std::cout << (checks - failures) << "/" << checks << " checks passed\n";
    return failures == 0 ? 0 : 1;
}
