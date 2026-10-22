// Minimal dependency-free test runner.
#include "CLIParser.hpp"
#include "Completion.hpp"
#include "Replace.hpp"
#include "Format.hpp"
#include "IgnoreRules.hpp"
#include "SearchEngine.hpp"
#include "Utf8.hpp"
#include "Wildcard.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
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

using Ranges = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

void testHighlight() {
    // Literal, case-insensitive, several hits.
    CHECK((pfind::Matcher::literal("todo").findAll("TODO a todo b") == Ranges{{0, 4}, {7, 11}}));

    // Case-sensitive.
    pfind::MatchOptions cs;
    cs.caseSensitive = true;
    CHECK((pfind::Matcher::literal("todo", cs).findAll("TODO a todo b") == Ranges{{7, 11}}));

    // Ranges are byte offsets into the ORIGINAL text, even when characters are multi-byte.
    // "Việt Nam": 'ệ' is 3 bytes, so "Nam" starts at byte 7.
    pfind::MatchOptions accents;
    accents.ignoreAccents = true;
    CHECK((pfind::Matcher::literal("nam", accents).findAll("Việt Nam") == Ranges{{7, 10}}));
    CHECK((pfind::Matcher::literal("viet", accents).findAll("Việt Nam") == Ranges{{0, 6}}));

    // Decomposed (NFD) text: the combining mark belongs to the match.
    const std::string nfd = "Vie\xCC\xA3\xCC\x82t Nam";   // V i e + dot below + circumflex t
    CHECK((pfind::Matcher::literal("viet", accents).findAll(nfd) == Ranges{{0, 8}}));

    // Regular expressions: byte offsets, empty matches skipped.
    pfind::MatchOptions rx;
    rx.regex = true;
    CHECK((pfind::Matcher("a+", rx).findAll("baaab aa") == Ranges{{1, 4}, {6, 8}}));
    CHECK(pfind::Matcher("x*", rx).findAll("abc").empty());
    rx.ignoreAccents = true;
    CHECK(pfind::Matcher("a+", rx).findAll("baaab").empty());       // not supported => no ranges

    // Wildcards produce nothing.
    CHECK(pfind::Matcher("*.cpp").findAll("main.cpp").empty());

    // Range cap.
    CHECK(pfind::Matcher::literal("a").findAll(std::string(500, 'a')).size() == 100);
}

void testReplaceAll() {
    std::size_t count = 0;

    // Literal: case-insensitive hits are all replaced by the verbatim text ('$' has no meaning).
    CHECK(pfind::Matcher::literal("todo").replaceAll("TODO a todo b", "$1!", count) == "$1! a $1! b");
    CHECK(count == 2);
    CHECK(pfind::Matcher::literal("zzz").replaceAll("abc", "x", count) == "abc");
    CHECK(count == 0);

    pfind::MatchOptions cs;
    cs.caseSensitive = true;
    CHECK(pfind::Matcher::literal("todo", cs).replaceAll("TODO a todo", "x", count) == "TODO a x");

    // Unicode and accents: ranges map back to the original bytes.
    pfind::MatchOptions accents;
    accents.ignoreAccents = true;
    CHECK(pfind::Matcher::literal("viet", accents).replaceAll("Việt Nam, VIỆT", "VN", count) == "VN Nam, VN");
    CHECK(count == 2);
    CHECK(pfind::Matcher::literal("việt").replaceAll("Việt Nam", "Viet", count) == "Viet Nam");

    // Regex: capture groups, whole match, '$$', empty matches ignored.
    pfind::MatchOptions rx;
    rx.regex = true;
    rx.caseSensitive = true;
    CHECK(pfind::Matcher("(\\w+)@(\\w+)", rx).replaceAll("ana@home", "$2:$1", count) == "home:ana");
    CHECK(pfind::Matcher("\\d+", rx).replaceAll("a1 b22", "<$&>", count) == "a<1> b<22>");
    CHECK(count == 2);
    CHECK(pfind::Matcher("\\d+", rx).replaceAll("a1", "$$", count) == "a$");
    CHECK(pfind::Matcher("x*", rx).replaceAll("abc", "-", count) == "abc");
    CHECK(count == 0);
}

std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void testReplaceInFile() {
    const fs::path dir = fs::temp_directory_path() / "pfind_test_replace";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const auto write = [&](const char* name, const std::string& content) {
        const fs::path path = dir / name;
        std::ofstream(path, std::ios::binary) << content;
        return path;
    };

    const pfind::Matcher todo = pfind::Matcher::literal("todo");

    // Dry run: reports the changes, leaves the file alone.
    {
        const auto path = write("a.txt", "one\nTODO two\nthree todo todo\n");
        const auto result = pfind::replaceInFile(path, todo, "DONE", false, false);
        CHECK(result.error.empty());
        CHECK(result.replacements == 3);
        CHECK(result.changes.size() == 2);
        CHECK(result.changes.size() == 2 && result.changes[0].line == 2);
        CHECK(result.changes.size() == 2 && result.changes[0].before == "TODO two");
        CHECK(result.changes.size() == 2 && result.changes[0].after == "DONE two");
        CHECK(!result.written);
        CHECK(slurp(path) == "one\nTODO two\nthree todo todo\n");
    }

    // Apply: file rewritten, backup kept, no temporary file left behind.
    {
        const auto path = write("b.txt", "one\nTODO two\n");
        const auto result = pfind::replaceInFile(path, todo, "DONE", true, true);
        CHECK(result.error.empty() && result.written);
        CHECK(slurp(path) == "one\nDONE two\n");
        CHECK(slurp(dir / "b.txt.bak") == "one\nTODO two\n");
        CHECK(!fs::exists(dir / "b.txt.pfind-tmp"));
    }

    // CRLF endings and a missing final newline are preserved byte for byte.
    {
        const auto path = write("c.txt", "TODO a\r\nplain\r\nlast TODO");
        const auto result = pfind::replaceInFile(path, todo, "X", true, false);
        CHECK(result.error.empty() && result.written);
        CHECK(slurp(path) == "X a\r\nplain\r\nlast X");
    }

    // Nothing to change (replacement equals the text): no write at all.
    {
        const auto path = write("d.txt", "TODO\n");
        const auto before = fs::last_write_time(path);
        const auto result = pfind::replaceInFile(path, todo, "TODO", true, true);
        CHECK(result.changes.empty() && !result.written);
        CHECK(fs::last_write_time(path) == before);
        CHECK(!fs::exists(dir / "d.txt.bak"));
    }

    // Binary files are skipped; invalid UTF-8 text keeps its bytes.
    {
        const auto path = write("e.bin", std::string("TODO\0data", 9));
        const auto result = pfind::replaceInFile(path, todo, "X", true, false);
        CHECK(result.changes.empty() && !result.written);
        CHECK(slurp(path) == std::string("TODO\0data", 9));

        const auto latin = write("f.txt", "caf\xE9 TODO\n");
        pfind::replaceInFile(latin, todo, "ok", true, false);
        CHECK(slurp(latin) == "caf\xE9 ok\n");
    }

    // Permissions are kept.
#ifndef _WIN32
    {
        const auto path = write("g.sh", "TODO\n");
        fs::permissions(path, fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read);
        const auto mode = fs::status(path).permissions();
        pfind::replaceInFile(path, todo, "x", true, false);
        CHECK(fs::status(path).permissions() == mode);
    }

    // A symbolic link stays a link; the target is what gets rewritten.
    {
        const auto target = write("target.txt", "TODO\n");
        std::error_code ec;
        fs::create_symlink(target, dir / "link.txt", ec);
        if (!ec) {
            const auto result = pfind::replaceInFile(dir / "link.txt", todo, "x", true, false);
            CHECK(result.written);
            CHECK(fs::is_symlink(dir / "link.txt"));
            CHECK(slurp(target) == "x\n");
        }
    }
#endif

    // Missing file: an error, no crash.
    {
        const auto result = pfind::replaceInFile(dir / "missing.txt", todo, "x", true, false);
        CHECK(!result.error.empty());
    }

    // Regex replacement with groups.
    {
        pfind::MatchOptions rx;
        rx.regex = true;
        rx.caseSensitive = true;
        const auto path = write("h.txt", "name: Ana\nname: Binh\n");
        pfind::replaceInFile(path, pfind::Matcher("name: (\\w+)", rx), "$1 is here", true, false);
        CHECK(slurp(path) == "Ana is here\nBinh is here\n");
    }

    fs::remove_all(dir);
}

std::string capture(void (*printer)(const char*)) {
    std::ostringstream buffer;
    std::streambuf* original = std::cout.rdbuf(buffer.rdbuf());
    printer("pfind");
    std::cout.rdbuf(original);
    return buffer.str();
}

void testOptionTable() {
    using pfind::CLIParser;

    // 1. Every entry of the table is accepted by the real parser.
    for (const auto& spec : pfind::optionSpecs()) {
        std::vector<std::string> args = {"pfind", ".", "-g", "x", "--replace", "y"};
        args.push_back(std::string(spec.longName));
        if (!spec.valueName.empty()) {
            args.push_back(std::string(spec.sample));
        }
        const auto parsed = CLIParser::parse(args);
        const bool unknown = parsed.error.find("Unknown option") != std::string::npos;
        if (unknown) {
            std::cerr << "option not understood by the parser: " << spec.longName << "\n";
        }
        CHECK(!unknown);

        if (!spec.shortName.empty()) {
            std::vector<std::string> shortArgs = {"pfind", ".", "-g", "x", "--replace", "y"};
            shortArgs.push_back(std::string(spec.shortName));
            if (!spec.valueName.empty()) {
                shortArgs.push_back(std::string(spec.sample));
            }
            const auto parsedShort = CLIParser::parse(shortArgs);
            CHECK(parsedShort.error.find("Unknown option") == std::string::npos);
        }
    }

    // 2. Every option mentioned in --help is in the table (so completion cannot miss one).
    const std::string help = capture(&CLIParser::printHelp);
    std::size_t found = 0;
    for (std::size_t pos = 0; (pos = help.find("--", pos)) != std::string::npos;) {
        std::size_t end = pos + 2;
        while (end < help.size() && (std::isalnum(static_cast<unsigned char>(help[end])) || help[end] == '-')) {
            ++end;
        }
        const std::string name = help.substr(pos, end - pos);
        pos = end;
        if (name.size() < 3) continue;

        bool known = false;
        for (const auto& spec : pfind::optionSpecs()) {
            known = known || spec.longName == name;
        }
        if (!known) {
            std::cerr << "--help mentions an option missing from the table: " << name << "\n";
        }
        CHECK(known);
        ++found;
    }
    CHECK(found > 30);

    // 3. ... and every table entry is documented in --help.
    for (const auto& spec : pfind::optionSpecs()) {
        const bool documented = help.find(std::string(spec.longName)) != std::string::npos;
        if (!documented) {
            std::cerr << "option missing from --help: " << spec.longName << "\n";
        }
        CHECK(documented);
    }
}

void testCompletionScripts() {
    for (const char* shell : {"bash", "zsh", "powershell"}) {
        const std::string script = pfind::completionScript(shell);
        CHECK(script.size() > 500);
        for (const auto& spec : pfind::optionSpecs()) {
            CHECK(script.find(std::string(spec.longName)) != std::string::npos);
        }
        for (const char* placeholder : {"@CHOICES@", "@FREE@", "@OPTIONS@", "@ENTRIES@"}) {
            CHECK(script.find(placeholder) == std::string::npos);   // everything was substituted
        }
    }

    CHECK(pfind::completionScript("bash").find("complete -F _pfind pfind") != std::string::npos);
    CHECK(pfind::completionScript("zsh").rfind("#compdef pfind", 0) == 0);
    CHECK(pfind::completionScript("powershell").find("Register-ArgumentCompleter") != std::string::npos);
    CHECK(pfind::completionScript("fish").empty());

    // Choices are completed.
    CHECK(pfind::completionScript("bash").find("file dir all") != std::string::npos);
    CHECK(pfind::completionScript("zsh").find("(name size time)") != std::string::npos);
    CHECK(pfind::completionScript("powershell").find("'always'") != std::string::npos);

    // The parser accepts --completion.
    auto result = pfind::CLIParser::parse({"pfind", "--completion", "bash"});
    CHECK(result.valid && result.completionShell == "bash");
    CHECK(!pfind::CLIParser::parse({"pfind", "--completion", "fish"}).valid);
    CHECK(!pfind::CLIParser::parse({"pfind", "--completion"}).valid);
}

void testReplaceCli() {
    using pfind::CLIParser;

    auto ok = CLIParser::parse({"pfind", ".", "-g", "old", "--replace", "new"});
    CHECK(ok.valid && ok.replace.text == "new" && !ok.replace.apply && !ok.replace.backup);
    auto applied = CLIParser::parse({"pfind", ".", "-g", "old", "--replace=new", "--apply", "--backup"});
    CHECK(applied.valid && applied.replace.apply && applied.replace.backup);
    auto empty = CLIParser::parse({"pfind", ".", "-g", "old", "--replace", ""});   // delete the text
    CHECK(empty.valid && empty.replace.text == "");

    CHECK(!CLIParser::parse({"pfind", ".", "--replace", "x"}).valid);                       // needs --grep
    CHECK(!CLIParser::parse({"pfind", ".", "-g", "a", "--apply"}).valid);                   // needs --replace
    CHECK(!CLIParser::parse({"pfind", ".", "-g", "a", "--backup"}).valid);
    CHECK(!CLIParser::parse({"pfind", ".", "-g", "a", "--replace", "b", "--json"}).valid);
    CHECK(!CLIParser::parse({"pfind", ".", "-g", "a", "--replace", "b", "-c"}).valid);
    CHECK(!CLIParser::parse({"pfind", ".", "-g", "a", "--replace", "b", "-l"}).valid);
    CHECK(!CLIParser::parse({"pfind", ".", "-g", "a", "--replace", "b", "-C", "1"}).valid);
    CHECK(!CLIParser::parse({"pfind", ".", "-g", "a", "-E", "--ignore-accents", "--replace", "b"}).valid);
    CHECK(CLIParser::parse({"pfind", ".", "-g", "a", "--ignore-accents", "--replace", "b"}).valid);
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

using Verdict = pfind::IgnoreRules::Verdict;

// Convenience: checks a path like "src/a.log" against rules parsed from `text`.
Verdict ignoreCheck(const char* rules, const char* path, bool isDir = false) {
    const std::string p = path;
    const auto slash = p.rfind('/');
    const std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
    return pfind::IgnoreRules::parse(rules).check(p, name, isDir);
}

void testIgnoreRules() {
    // Basics: name patterns match at any depth.
    CHECK(ignoreCheck("*.log", "a.log") == Verdict::Ignored);
    CHECK(ignoreCheck("*.log", "deep/er/a.log") == Verdict::Ignored);
    CHECK(ignoreCheck("*.log", "a.txt") == Verdict::None);
    CHECK(ignoreCheck("node_modules", "x/node_modules", true) == Verdict::Ignored);

    // Comments, blank lines, CRLF, trailing spaces, BOM.
    CHECK(ignoreCheck("# *.log\n\n", "a.log") == Verdict::None);
    CHECK(ignoreCheck("*.log\r\n", "a.log") == Verdict::Ignored);
    CHECK(ignoreCheck("*.log   \n", "a.log") == Verdict::Ignored);
    CHECK(ignoreCheck("\xEF\xBB\xBF*.log\n", "a.log") == Verdict::Ignored);
    CHECK(ignoreCheck("\\#notes\n", "#notes") == Verdict::Ignored);
    CHECK(ignoreCheck("\\!bang\n", "!bang") == Verdict::Ignored);

    // Directory-only and anchored patterns.
    CHECK(ignoreCheck("build/", "build", true) == Verdict::Ignored);
    CHECK(ignoreCheck("build/", "build", false) == Verdict::None);       // a file called "build"
    CHECK(ignoreCheck("/build", "build", true) == Verdict::Ignored);
    CHECK(ignoreCheck("/build", "sub/build", true) == Verdict::None);    // anchored to the top
    CHECK(ignoreCheck("docs/gen", "docs/gen", true) == Verdict::Ignored);
    CHECK(ignoreCheck("docs/gen", "x/docs/gen", true) == Verdict::None); // '/' inside => anchored

    // Wildcards never cross '/'.
    CHECK(ignoreCheck("src/*.o", "src/a.o") == Verdict::Ignored);
    CHECK(ignoreCheck("src/*.o", "src/deep/a.o") == Verdict::None);
    CHECK(ignoreCheck("file?.txt", "file1.txt") == Verdict::Ignored);
    CHECK(ignoreCheck("file?.txt", "file10.txt") == Verdict::None);
    CHECK(ignoreCheck("[a-c]x", "bx") == Verdict::Ignored);
    CHECK(ignoreCheck("[!a-c]x", "bx") == Verdict::None);
    CHECK(ignoreCheck("[!a-c]x", "zx") == Verdict::Ignored);

    // Double star.
    CHECK(ignoreCheck("**/temp", "temp", true) == Verdict::Ignored);
    CHECK(ignoreCheck("**/temp", "a/b/temp", true) == Verdict::Ignored);
    CHECK(ignoreCheck("**/temp", "a/b/temporary", true) == Verdict::None);
    CHECK(ignoreCheck("logs/**", "logs/a/b.txt") == Verdict::Ignored);
    CHECK(ignoreCheck("logs/**", "logs", true) == Verdict::None);        // contents only
    CHECK(ignoreCheck("docs/**/*.md", "docs/a.md") == Verdict::Ignored); // zero directories
    CHECK(ignoreCheck("docs/**/*.md", "docs/x/y/a.md") == Verdict::Ignored);
    CHECK(ignoreCheck("docs/**/*.md", "other/a.md") == Verdict::None);

    // Negation and ordering: the last matching rule wins.
    CHECK(ignoreCheck("*.log\n!keep.log\n", "keep.log") == Verdict::Whitelisted);
    CHECK(ignoreCheck("*.log\n!keep.log\n", "other.log") == Verdict::Ignored);
    CHECK(ignoreCheck("!keep.log\n*.log\n", "keep.log") == Verdict::Ignored);

    // Unicode names; '?' is one character.
    CHECK(ignoreCheck("tài?liệu", "tài liệu") == Verdict::Ignored);
    CHECK(ignoreCheck("*.tmp", "Báo cáo.tmp") == Verdict::Ignored);

    // Many stars must not blow up.
    CHECK(ignoreCheck("*a*a*a*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac") == Verdict::None);
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

        // Highlight ranges are recorded only on request, and only for real matches.
        {
            auto h = base;
            h.patterns = {"one.txt"};
            h.contentPattern = "todo";
            h.highlightMatches = true;
            h.jobs = 1;
            pfind::SearchEngine hEngine(h);
            std::vector<pfind::FileInfo> hHits;
            hEngine.search([&](const pfind::FileInfo& info) { hHits.push_back(info); });
            CHECK(hHits.size() == 1);
            if (hHits.size() == 1) {
                CHECK(hHits[0].matches.size() == 2);
                CHECK((hHits[0].matches[0].ranges == Ranges{{0, 4}}));   // "TODO: fix this"
                CHECK((hHits[0].matches[1].ranges == Ranges{{5, 9}}));   // "last TODO here"
            }

            h.highlightMatches = false;
            pfind::SearchEngine plainEngine(h);
            plainEngine.search([&](const pfind::FileInfo& info) { CHECK(info.matches[0].ranges.empty()); });
        }

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

    {
        // .gitignore support on a small fake repository.
        const fs::path repo = fs::temp_directory_path() / "pfind_test_repo";
        fs::remove_all(repo);

        touch(repo / ".git" / "HEAD", "ref: refs/heads/main\n");
        touch(repo / ".git" / "info" / "exclude", "secret.txt\n");
        touch(repo / ".gitignore", "*.log\nbuild/\n/only-top.txt\n!important.log\n");
        touch(repo / "app.cpp");
        touch(repo / "debug.log");
        touch(repo / "important.log");
        touch(repo / "secret.txt");
        touch(repo / "only-top.txt");
        touch(repo / "build" / "out.o");
        touch(repo / "src" / "only-top.txt");
        touch(repo / "src" / "main.cpp");
        touch(repo / "src" / "trace.log");
        touch(repo / "src" / ".gitignore", "generated/\n*.tmp\n!keep.tmp\n");
        touch(repo / "src" / "generated" / "g.cpp");
        touch(repo / "src" / "a.tmp");
        touch(repo / "src" / "keep.tmp");
        touch(repo / ".hidden" / "h.txt");
        touch(repo / "docs" / ".dotfile");
        touch(repo / "docs" / "guide.md");
        touch(repo / "docs" / "notes.md");
        touch(repo / "docs" / ".ignore", "notes.md\n");

        pfind::SearchOptions plain;
        plain.root = repo;

        // Without --gitignore nothing is skipped.
        CHECK(names(plain).count("debug.log") == 1);
        CHECK(names(plain).count("g.cpp") == 1);

        auto git = plain;
        git.useGitignore = true;
        const auto visible = names(git);
        CHECK(visible.count("app.cpp") == 1);
        CHECK(visible.count("debug.log") == 0);        // *.log
        CHECK(visible.count("trace.log") == 0);        // *.log applies in subdirectories too
        CHECK(visible.count("important.log") == 1);    // re-included with !
        CHECK(visible.count("out.o") == 0);            // build/
        CHECK(visible.count("secret.txt") == 0);       // .git/info/exclude
        CHECK(visible.count("g.cpp") == 0);            // src/.gitignore: generated/
        CHECK(visible.count("a.tmp") == 0);
        CHECK(visible.count("keep.tmp") == 1);
        CHECK(visible.count("HEAD") == 0);             // .git is never entered
        CHECK(visible.count("main.cpp") == 1);
        CHECK(visible.count("h.txt") == 1);            // hidden files are not "ignored" by default
        CHECK(visible.count("notes.md") == 1);         // .ignore is only read with --ignore-file

        // "/only-top.txt" is anchored: only the top-level file is ignored.
        std::size_t onlyTopCount = 0;
        {
            auto o = git;
            o.patterns = {"only-top.txt"};
            names(o, &onlyTopCount);
        }
        CHECK(onlyTopCount == 1);

        // Searching inside the repository still honours the parent's .gitignore.
        {
            auto sub = git;
            sub.root = repo / "src";
            const auto inSrc = names(sub);
            CHECK(inSrc.count("trace.log") == 0);       // from ../.gitignore
            CHECK(inSrc.count("g.cpp") == 0);           // from src/.gitignore
            CHECK(inSrc.count("main.cpp") == 1);
        }

        // Root given in a non-normalised form ("repo/src/../src/").
        {
            auto sub = git;
            sub.root = repo / "src" / ".." / "src";
            CHECK(names(sub).count("trace.log") == 0);
        }

        // Custom ignore file names.
        {
            auto custom = plain;
            custom.ignoreFiles = {".ignore"};
            const auto r = names(custom);
            CHECK(r.count("notes.md") == 0);
            CHECK(r.count("guide.md") == 1);
            CHECK(r.count("debug.log") == 1);           // .gitignore not requested
        }

        // Hidden files.
        {
            auto hidden = plain;
            hidden.skipHidden = true;
            const auto r = names(hidden);
            CHECK(r.count("h.txt") == 0);               // inside .hidden/
            CHECK(r.count(".dotfile") == 0);
            CHECK(r.count("app.cpp") == 1);
        }

        // Directories are reported/pruned consistently.
        {
            auto dirs = git;
            dirs.type = pfind::EntryType::Directory;
            const auto r = names(dirs);
            CHECK(r.count("build") == 0);
            CHECK(r.count("generated") == 0);
            CHECK(r.count(".git") == 0);
            CHECK(r.count("src") == 1);
        }

        fs::remove_all(repo);
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
    testIgnoreRules();
    testHighlight();
    testReplaceAll();
    testReplaceInFile();
    testOptionTable();
    testCompletionScripts();
    testReplaceCli();
    testCli();
    testEngine();

    std::cout << (checks - failures) << "/" << checks << " checks passed\n";
    return failures == 0 ? 0 : 1;
}
