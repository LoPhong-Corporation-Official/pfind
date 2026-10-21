// Minimal dependency-free test runner.
#include "CLIParser.hpp"
#include "Format.hpp"
#include "SearchEngine.hpp"
#include "Utf8.hpp"
#include "Wildcard.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>
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
    CHECK(!CLIParser::parse({"pfind", ".", "-l"}).valid);                      // needs --grep
    CHECK(!CLIParser::parse({"pfind", ".", "--max-lines", "3"}).valid);        // needs --grep
    CHECK(!CLIParser::parse({"pfind", ".", "-g", "x", "--type", "dir"}).valid);
    CHECK(CLIParser::parse({"pfind", ".", "-g", "x", "-l", "-E"}).valid);
    {
        auto c = CLIParser::parse({"pfind", ".", "-g", "x", "-A", "2", "-B", "3", "-j", "4"});
        CHECK(c.valid && c.options.afterContext == 2 && c.options.beforeContext == 3 && c.options.jobs == 4u);
        auto both = CLIParser::parse({"pfind", ".", "-g", "x", "-C", "5"});
        CHECK(both.valid && both.options.afterContext == 5 && both.options.beforeContext == 5);
    }
    CHECK(CLIParser::parse({"pfind", ".", "--color=always"}).output.color == pfind::ColorMode::Always);
    CHECK(CLIParser::parse({"pfind", ".", "--color", "never"}).output.color == pfind::ColorMode::Never);
    CHECK(!CLIParser::parse({"pfind", ".", "--color=rainbow"}).valid);
    CHECK(!CLIParser::parse({"pfind", ".", "-C", "2"}).valid);                 // needs --grep
    CHECK(!CLIParser::parse({"pfind", ".", "-j", "2"}).valid);                 // needs --grep
    CHECK(!CLIParser::parse({"pfind", ".", "-g", "x", "-j", "0"}).valid);
    CHECK(!CLIParser::parse({"pfind", ".", "-g", "x", "-C", "99999"}).valid);
    CHECK(!CLIParser::parse({"pfind", ".", "-E"}).valid);                      // needs --grep

    // "--" lets a pattern start with a dash.
    auto dash = CLIParser::parse({"pfind", ".", "--", "-weird*"});
    CHECK(dash.valid && dash.options.patterns.size() == 1);
}

void testRegex() {
    pfind::MatchOptions options;
    options.regex = true;

    CHECK(pfind::Matcher("^main\\.(cpp|hpp)$", options).matches("main.cpp"));
    CHECK(!pfind::Matcher("^main\\.(cpp|hpp)$", options).matches("main.c"));
    CHECK(pfind::Matcher("\\d{4}-\\d{2}", options).matches("report-2026-10.txt"));  // search semantics
    CHECK(pfind::Matcher("README", options).matches("readme.md"));                      // case-insensitive
    options.caseSensitive = true;
    CHECK(!pfind::Matcher("README", options).matches("readme.md"));

    options.ignoreAccents = true;
    CHECK(pfind::Matcher("^Bao cao", options).matches("Báo cáo.txt"));

    bool threw = false;
    try {
        pfind::Matcher bad("([unclosed", options);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    // Literal substring helper escapes wildcard characters.
    CHECK(pfind::Matcher::literal("a*b").matches("xx a*b yy"));
    CHECK(!pfind::Matcher::literal("a*b").matches("xx aXb yy"));
    CHECK(pfind::Matcher::literal("TODO").matches("// todo: fix"));

    // Fast paths must agree with the general matcher.
    {
        pfind::MatchOptions cs;
        cs.caseSensitive = true;
        CHECK(!pfind::Matcher::literal("TODO", cs).matches("// todo: fix"));
        CHECK(pfind::Matcher::literal("TODO", cs).matches("// TODO: fix"));
        CHECK(!pfind::Matcher::literal("needle").matches("need"));      // needle longer than text
        CHECK(pfind::Matcher::literal("needle").matches("needle"));
        CHECK(!pfind::Matcher::literal("needle").matches(""));
        CHECK(pfind::Matcher::literal("abc").matches("xxabxabc"));      // partial overlap before the hit

        pfind::MatchOptions accents;
        accents.ignoreAccents = true;
        CHECK(pfind::Matcher::literal("cafe", accents).matches("Quán CAFÉ nhỏ"));     // ASCII needle, accented text
        CHECK(pfind::Matcher::literal("liệu", accents).matches("Tai LIEU"));          // accented needle, plain text
        CHECK(!pfind::Matcher::literal("cafe").matches("Quán CAFÉ nhỏ"));
        CHECK(pfind::Matcher::literal("CAFÉ").matches("quán café nhỏ"));              // Unicode case folding
    }
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

    {
        // Content search.
        touch(root / "notes" / "one.txt", "first line\nTODO: fix this\r\nlast TODO here\n");
        touch(root / "notes" / "two.txt", "nothing to see\n");
        touch(root / "notes" / "three.txt", "Cần làm: sửa lỗi\n");
        touch(root / "notes" / "bin.dat", std::string("TODO\0binary", 11));

        auto options = base;
        options.patterns = {"*.txt", "*.dat"};
        options.contentPattern = "todo";
        CHECK((names(options) == std::set<std::string>{"one.txt"}));  // binary file skipped

        pfind::SearchEngine engine(options);
        std::vector<pfind::FileInfo> hits;
        engine.search([&](const pfind::FileInfo& info) { hits.push_back(info); });
        CHECK(hits.size() == 1);
        CHECK(hits.size() == 1 && hits[0].matches.size() == 2);
        CHECK(hits.size() == 1 && hits[0].matches[0].line == 2);
        CHECK(hits.size() == 1 && hits[0].matches[0].text == "TODO: fix this");  // CR stripped

        options.filesWithMatches = true;
        pfind::SearchEngine first(options);
        std::size_t perFile = 0;
        first.search([&](const pfind::FileInfo& info) { perFile = info.matches.size(); });
        CHECK(perFile == 1);

        options.filesWithMatches = false;
        options.maxLinesPerFile = 1;
        pfind::SearchEngine limited(options);
        limited.search([&](const pfind::FileInfo& info) { perFile = info.matches.size(); });
        CHECK(perFile == 1);

        auto accents = base;
        accents.patterns = {"*.txt"};
        accents.contentPattern = "can lam: sua loi";
        CHECK(names(accents).empty());
        accents.ignoreAccents = true;
        CHECK((names(accents) == std::set<std::string>{"three.txt"}));

        auto rx = base;
        rx.patterns = {"*.txt"};   // names stay wildcards; only the content is a regex
        rx.contentRegex = true;
        rx.contentPattern = "^last\\s+TODO";
        CHECK((names(rx) == std::set<std::string>{"one.txt"}));

        // Chunked line reader: lines that straddle the 256 KB read buffer, a last line without
        // a trailing newline, CRLF endings, empty files and over-long lines.
        touch(root / "edge" / "boundary.txt",
              std::string(262140, 'x') + "\nNEEDLE two\n" + std::string(300000, 'y') + "NEEDLE-end");
        touch(root / "edge" / "crlf.txt", "one\r\nNEEDLE two\r\nthree\r\n");
        touch(root / "edge" / "empty.txt", "");
        touch(root / "edge" / "longline.txt", std::string(2 * 1024 * 1024, 'a') + "\nNEEDLE\n");

        auto edge = base;
        edge.root = root / "edge";
        edge.contentPattern = "NEEDLE";
        edge.jobs = 1;
        std::map<std::string, std::vector<pfind::ContentMatch>> edgeHits;
        pfind::SearchEngine edgeEngine(edge);
        edgeEngine.search([&](const pfind::FileInfo& info) {
            edgeHits[pfind::utf8::fromPath(info.path.filename())] = info.matches;
        });
        CHECK(edgeHits.size() == 2);   // empty.txt and longline.txt are not reported
        CHECK(edgeHits["boundary.txt"].size() == 2);
        CHECK(edgeHits["boundary.txt"].size() == 2 && edgeHits["boundary.txt"][0].line == 2);
        CHECK(edgeHits["boundary.txt"].size() == 2 && edgeHits["boundary.txt"][1].line == 3);
        CHECK(edgeHits["crlf.txt"].size() == 1 && edgeHits["crlf.txt"][0].text == "NEEDLE two");

        // Context lines.
        touch(root / "ctx" / "c.txt", "a\nb\nTODO c\nd\ne\nf\nTODO g\nh\n");
        auto ctx = base;
        ctx.patterns = {"c.txt"};
        ctx.contentPattern = "TODO";
        ctx.beforeContext = 1;
        ctx.afterContext = 1;
        std::vector<pfind::FileInfo> ctxHits;
        pfind::SearchEngine ctxEngine(ctx);
        ctxEngine.search([&](const pfind::FileInfo& info) { ctxHits.push_back(info); });
        CHECK(ctxHits.size() == 1);
        if (ctxHits.size() == 1) {
            const auto& m = ctxHits[0].matches;
            CHECK(m.size() == 6);
            const std::size_t lines[] = {2, 3, 4, 6, 7, 8};
            const bool contexts[] = {true, false, true, true, false, true};
            for (std::size_t i = 0; i < m.size() && i < 6; ++i) {
                CHECK(m[i].line == lines[i]);
                CHECK(m[i].context == contexts[i]);
            }
        }

        // maxLinesPerFile counts matches only; trailing context is still delivered.
        ctx.maxLinesPerFile = 1;
        pfind::SearchEngine ctxLimited(ctx);
        std::size_t shown = 0;
        ctxLimited.search([&](const pfind::FileInfo& info) { shown = info.matches.size(); });
        CHECK(shown == 3);  // b, TODO c, d

        auto badRx = rx;
        badRx.contentPattern = "([";
        bool threw = false;
        try { pfind::SearchEngine broken(badRx); } catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
    }

    {
        auto options = base;
        options.regex = true;
        options.patterns = {"^[a-d]\\.cpp$"};
        CHECK((names(options) == std::set<std::string>{"a.cpp", "c.cpp", "d.cpp"}));
    }

    {
        // Parallel scanning must give exactly the same results, in the same order, as one thread.
        const fs::path bulk = root / "bulk";
        for (int i = 0; i < 400; ++i) {
            touch(bulk / ("dir" + std::to_string(i % 7)) / ("f" + std::to_string(i) + ".txt"),
                  (i % 3 == 0) ? "needle here\n" : "nothing\n");
        }

        auto collect = [&](unsigned jobs, std::optional<std::size_t> limit) {
            auto options = base;
            options.root = bulk;
            options.contentPattern = "needle";
            options.jobs = jobs;
            options.maxResults = limit;
            std::vector<std::string> paths;
            pfind::SearchEngine engine(options);
            engine.search([&](const pfind::FileInfo& info) { paths.push_back(pfind::utf8::fromPath(info.path)); });
            return paths;
        };

        const auto single = collect(1, std::nullopt);
        CHECK(single.size() == 134);
        CHECK(collect(4, std::nullopt) == single);
        CHECK(collect(8, std::nullopt) == single);

        const auto limitedSingle = collect(1, 5);
        CHECK(limitedSingle.size() == 5);
        CHECK(collect(4, 5) == limitedSingle);
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
    testRegex();
    testCli();
    testEngine();

    std::cout << (checks - failures) << "/" << checks << " checks passed\n";
    return failures == 0 ? 0 : 1;
}
