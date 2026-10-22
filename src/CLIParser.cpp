#include "CLIParser.hpp"

#include "Completion.hpp"
#include "Format.hpp"
#include "Utf8.hpp"

#include <iostream>
#include <optional>
#include <string_view>

namespace pfind {
namespace {

struct ParseError {
    std::string message;
};

[[noreturn]] void fail(std::string message) {
    throw ParseError{std::move(message)};
}

bool is(std::string_view name, std::string_view shortName, std::string_view longName) {
    return name == shortName || name == longName;
}

} // namespace

CLIResult CLIParser::parse(int argc, char* argv[]) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return parse(args);
}

CLIResult CLIParser::parse(const std::vector<std::string>& args) {
    CLIResult result;
    SearchOptions& options = result.options;
    OutputOptions& output = result.output;
    ReplaceOptions& replace = result.replace;

    if (args.size() <= 1) {
        result.showHelp = true;
        return result;
    }

    try {
        bool endOfOptions = false;
        bool haveRoot = false;

        for (std::size_t i = 1; i < args.size(); ++i) {
            const std::string_view arg = args[i];

            if (endOfOptions || arg.size() < 2 || arg.front() != '-') {
                if (!haveRoot) {
                    options.root = utf8::toPath(arg);
                    haveRoot = true;
                } else {
                    options.patterns.emplace_back(arg);
                }
                continue;
            }

            if (arg == "--") {
                endOfOptions = true;
                continue;
            }

            // Support both "--name value" and "--name=value".
            std::string_view name = arg;
            std::optional<std::string_view> inlineValue;
            if (arg.starts_with("--")) {
                const auto eq = arg.find('=');
                if (eq != std::string_view::npos) {
                    name = arg.substr(0, eq);
                    inlineValue = arg.substr(eq + 1);
                }
            }

            auto value = [&]() -> std::string_view {
                if (inlineValue) {
                    return *inlineValue;
                }
                if (++i >= args.size()) {
                    fail("Missing value for " + std::string(name) + ".");
                }
                return args[i];
            };

            auto noValue = [&]() {
                if (inlineValue) {
                    fail("Option " + std::string(name) + " does not take a value.");
                }
            };

            if (is(name, "-h", "--help")) {
                noValue();
                result.showHelp = true;
                return result;
            }
            if (is(name, "-v", "--version")) {
                noValue();
                result.showVersion = true;
                return result;
            }

            if (name == "--completion") {
                const std::string_view v = value();
                if (!isCompletionShell(v)) {
                    fail("Invalid value for --completion: '" + std::string(v) + "' (use bash, zsh or powershell).");
                }
                result.completionShell = std::string(v);
                return result;
            }

            if (name == "--apply")  { noValue(); replace.apply = true;  continue; }
            if (name == "--backup") { noValue(); replace.backup = true; continue; }

            if (name == "--replace") {
                replace.text = std::string(value());
                continue;
            }

            if (is(name, "-i", "--case-insensitive")) { noValue(); options.caseSensitive = false; continue; }
            if (is(name, "-s", "--case-sensitive"))   { noValue(); options.caseSensitive = true;  continue; }
            if (name == "--ignore-accents")           { noValue(); options.ignoreAccents = true;  continue; }
            if (is(name, "-r", "--regex"))            { noValue(); options.regex = true;          continue; }
            if (is(name, "-E", "--grep-regex"))       { noValue(); options.contentRegex = true;   continue; }
            if (is(name, "-l", "--files-with-matches")) { noValue(); options.filesWithMatches = true; continue; }
            if (name == "--gitignore")                { noValue(); options.useGitignore = true;   continue; }
            if (name == "--skip-hidden")              { noValue(); options.skipHidden = true;     continue; }
            if (name == "--full-path")                { noValue(); options.matchFullPath = true;  continue; }
            if (name == "--follow-symlinks")          { noValue(); options.followSymlinks = true; continue; }
            if (name == "--details")                  { noValue(); options.showDetails = true;    continue; }
            if (name == "--all")                      { noValue(); options.type = EntryType::Any; continue; }

            if (is(name, "-c", "--count"))   { noValue(); output.countOnly = true; continue; }
            if (is(name, "-q", "--quiet"))   { noValue(); output.quiet = true;     continue; }
            if (name == "--verbose")         { noValue(); output.verbose = true;   continue; }
            if (name == "--json")            { noValue(); output.json = true;      continue; }
            if (name == "--print0")          { noValue(); output.null = true;      continue; }
            if (name == "--reverse")         { noValue(); output.reverse = true;   continue; }

            if (is(name, "-p", "--pattern")) {
                options.patterns.emplace_back(value());
                continue;
            }

            if (is(name, "-x", "--exclude")) {
                options.excludes.emplace_back(value());
                continue;
            }

            if (is(name, "-g", "--grep")) {
                const std::string_view v = value();
                if (v.empty()) {
                    fail("--grep needs a non-empty text.");
                }
                options.contentPattern = std::string(v);
                continue;
            }

            if (is(name, "-A", "--after-context") || is(name, "-B", "--before-context") ||
                is(name, "-C", "--context")) {
                const std::string_view v = value();
                const auto lines = parseUnsigned(v);
                if (!lines || *lines > 1000) {
                    fail("Invalid value for " + std::string(name) + ": '" + std::string(v) +
                         "' (use 0 to 1000).");
                }
                if (name != "-B" && name != "--before-context") options.afterContext = *lines;
                if (name != "-A" && name != "--after-context")  options.beforeContext = *lines;
                continue;
            }

            if (is(name, "-j", "--jobs")) {
                const std::string_view v = value();
                const auto jobs = parseUnsigned(v);
                if (!jobs || *jobs == 0 || *jobs > 256) {
                    fail("Invalid value for --jobs: '" + std::string(v) + "' (use 1 to 256).");
                }
                options.jobs = static_cast<unsigned>(*jobs);
                continue;
            }

            if (name == "--max-lines") {
                const std::string_view v = value();
                const auto limit = parseUnsigned(v);
                if (!limit || *limit == 0) {
                    fail("Invalid value for --max-lines: '" + std::string(v) + "' (use 1 or more).");
                }
                options.maxLinesPerFile = *limit;
                continue;
            }

            if (name == "--ignore-file") {
                const std::string_view v = value();
                if (v.empty() || v.find('/') != std::string_view::npos || v.find('\\') != std::string_view::npos) {
                    fail("--ignore-file takes a file name (like .ignore), not a path.");
                }
                options.ignoreFiles.emplace_back(v);
                continue;
            }

            if (name == "--type") {
                const std::string_view v = value();
                if (v == "f" || v == "file")       options.type = EntryType::File;
                else if (v == "d" || v == "dir")   options.type = EntryType::Directory;
                else if (v == "a" || v == "all")   options.type = EntryType::Any;
                else fail("Invalid value for --type: '" + std::string(v) + "' (use file, dir or all).");
                continue;
            }

            if (is(name, "-d", "--max-depth")) {
                const std::string_view v = value();
                const auto depth = parseUnsigned(v);
                if (!depth || *depth == 0 || *depth > 1000000) {
                    fail("Invalid value for --max-depth: '" + std::string(v) + "' (use 1 or more).");
                }
                options.maxDepth = static_cast<unsigned>(*depth);
                continue;
            }

            if (name == "--min-size" || name == "--max-size") {
                const std::string_view v = value();
                const auto size = parseSize(v);
                if (!size) {
                    fail("Invalid size for " + std::string(name) + ": '" + std::string(v) +
                         "' (examples: 500, 10K, 5M, 2G).");
                }
                (name == "--min-size" ? options.minSize : options.maxSize) = *size;
                continue;
            }

            if (name == "--modified-within") {
                const std::string_view v = value();
                const auto duration = parseDuration(v);
                if (!duration) {
                    fail("Invalid duration for --modified-within: '" + std::string(v) +
                         "' (examples: 30m, 12h, 7d, 2w).");
                }
                options.modifiedWithin = *duration;
                continue;
            }

            if (name == "--max-results") {
                const std::string_view v = value();
                const auto limit = parseUnsigned(v);
                if (!limit || *limit == 0) {
                    fail("Invalid value for --max-results: '" + std::string(v) + "' (use 1 or more).");
                }
                options.maxResults = *limit;
                continue;
            }

            if (name == "--color") {
                const std::string_view v = value();
                if (v == "auto")        output.color = ColorMode::Auto;
                else if (v == "always") output.color = ColorMode::Always;
                else if (v == "never")  output.color = ColorMode::Never;
                else fail("Invalid value for --color: '" + std::string(v) + "' (use auto, always or never).");
                continue;
            }

            if (name == "--sort") {
                const std::string_view v = value();
                if (v == "name")      output.sort = SortKey::Name;
                else if (v == "size") output.sort = SortKey::Size;
                else if (v == "time") output.sort = SortKey::Time;
                else fail("Invalid value for --sort: '" + std::string(v) + "' (use name, size or time).");
                continue;
            }

            fail("Unknown option: " + std::string(arg));
        }

        if (!haveRoot) {
            fail("Search path is required.");
        }

        if (options.minSize && options.maxSize && *options.minSize > *options.maxSize) {
            fail("--min-size must not be greater than --max-size.");
        }

        if (options.contentRegex && !options.contentPattern) {
            fail("--grep-regex needs --grep.");
        }
        if (options.filesWithMatches && !options.contentPattern) {
            fail("--files-with-matches needs --grep.");
        }
        if ((options.beforeContext > 0 || options.afterContext > 0) && !options.contentPattern) {
            fail("-A/-B/-C need --grep.");
        }
        if (options.jobs && !options.contentPattern) {
            fail("--jobs needs --grep.");
        }
        if (options.maxLinesPerFile && !options.contentPattern) {
            fail("--max-lines needs --grep.");
        }
        if (options.contentPattern && options.type == EntryType::Directory) {
            fail("--grep cannot be combined with --type dir.");
        }

        if ((replace.apply || replace.backup) && !replace.text) {
            fail("--apply and --backup need --replace.");
        }
        if (replace.text) {
            if (!options.contentPattern) {
                fail("--replace needs --grep (the text to find).");
            }
            if (options.contentRegex && options.ignoreAccents) {
                fail("--replace cannot combine --grep-regex with --ignore-accents.");
            }
            if (output.json || output.null || output.countOnly || options.filesWithMatches ||
                options.beforeContext > 0 || options.afterContext > 0 || options.maxLinesPerFile) {
                fail("--replace cannot be combined with --json, --print0, --count, "
                     "--files-with-matches, --max-lines or context options.");
            }
        }

        if (output.json && output.null) {
            fail("--json and --print0 cannot be combined.");
        }
    } catch (const ParseError& error) {
        result.valid = false;
        result.error = error.message;
    }

    return result;
}

void CLIParser::printHelp(const char* programName) {
    std::cout
        << "pfind - fast and portable file search tool\n\n"
        << "Usage:\n"
        << "  " << programName << " <path> [pattern ...] [options]\n\n"
        << "Examples:\n"
        << "  " << programName << " .\n"
        << "  " << programName << " . '*.cpp' '*.hpp'\n"
        << "  " << programName << " C:\\Projects '*.h' --details --sort size --reverse\n"
        << "  " << programName << " /home/user 'report*' --ignore-accents --modified-within 7d\n"
        << "  " << programName << " . '*.log' --min-size 10M -x node_modules -x .git\n"
        << "  " << programName << " . '*.tmp' --print0 | xargs -0 rm\n"
        << "  " << programName << " src '*.cpp' --grep TODO -C 2\n"
        << "  " << programName << " . '*.log' -g 'error|fatal' -E -l\n"
        << "  " << programName << " . -r '^test_.*\\.(cpp|hpp)$'\n\n"
        << "Matching:\n"
        << "  -p, --pattern <pattern>       Search pattern (repeatable; extra positional args work too)\n"
        << "  -x, --exclude <pattern>       Skip matching files and directories (repeatable)\n"
        << "  -i, --case-insensitive        Case-insensitive matching (default)\n"
        << "  -s, --case-sensitive          Case-sensitive matching\n"
        << "      --ignore-accents          'tai lieu' also matches 'Tài liệu'\n"
        << "  -r, --regex                   File name patterns and --exclude are regular expressions (ECMAScript)\n"
        << "      --full-path               Match against the path relative to <path>, not the name\n\n"
        << "Content search:\n"
        << "  -g, --grep <text>             Only files containing the text (binary files are skipped);\n"
        << "                                prints path:line: text for each matching line\n"
        << "  -E, --grep-regex              Treat the --grep text as a regular expression\n"
        << "  -l, --files-with-matches      With --grep, print only the file names\n"
        << "      --max-lines <n>           With --grep, show at most n matching lines per file\n"
        << "  -A, --after-context <n>       With --grep, also show n lines after each match\n"
        << "  -B, --before-context <n>      With --grep, also show n lines before each match\n"
        << "  -C, --context <n>             Both of the above\n"
        << "  -j, --jobs <n>                Threads used to scan file contents (default: automatic)\n\n"
        << "Replace (preview by default):\n"
        << "      --replace <text>          Replace every --grep match by <text>; shows what would change\n"
        << "                                (with -E, $1 $2 ... and $& refer to the match)\n"
        << "      --apply                   Actually write the changes (atomic, keeps permissions)\n"
        << "      --backup                  Keep a copy of each changed file as <file>.bak\n\n"
        << "Filtering:\n"
        << "      --type <file|dir|all>     Kind of entry to report (default: file)\n"
        << "      --all                     Same as --type all\n"
        << "  -d, --max-depth <n>           Descend at most n levels (1 = direct children)\n"
        << "      --min-size <size>         Only files of at least this size (e.g. 10K, 5M, 2G)\n"
        << "      --max-size <size>         Only files of at most this size\n"
        << "      --modified-within <time>  Only entries changed recently (e.g. 30m, 12h, 7d, 2w)\n"
        << "      --max-results <n>         Stop after n matches\n"
        << "      --follow-symlinks         Follow directory symbolic links (loops are detected)\n"
        << "      --gitignore               Skip what .gitignore files exclude (also parent directories inside\n"
        << "                                a git repository and .git/info/exclude); never enters .git\n"
        << "      --ignore-file <name>      Also read ignore files with this name in every directory\n"
        << "                                (same syntax as .gitignore, e.g. .ignore); repeatable\n"
        << "      --skip-hidden             Skip files and directories whose name starts with '.'\n\n"
        << "Output:\n"
        << "      --details                 Show size and modification time\n"
        << "      --color <auto|always|never>  Colour the output (default: auto = only on a terminal;\n"
        << "                                NO_COLOR is respected)\n"
        << "      --sort <name|size|time>   Sort results (collects all matches first)\n"
        << "      --reverse                 Reverse the sort order\n"
        << "      --json                    One JSON object per line\n"
        << "      --print0                  Separate paths with NUL characters\n"
        << "  -c, --count                   Print only the number of matches\n"
        << "  -q, --quiet                   No summary or warnings\n"
        << "      --verbose                 Print the effective search settings\n"
        << "      --completion <shell>      Print a completion script (bash, zsh or powershell)\n"
        << "  -h, --help                    Show this help\n"
        << "  -v, --version                 Show version\n\n"
        << "Wildcards:\n"
        << "  *       any number of characters\n"
        << "  ?       exactly one character\n"
        << "  [abc]   one of the listed characters     [a-z]  a range     [!abc]  none of them\n"
        << "  \\x      the literal character x\n\n"
        << "Exit status:\n"
        << "  0  at least one match     1  no matches     2  error (bad usage or unreadable path)\n";
}

void CLIParser::printVersion() {
    std::cout << "pfind " << PFIND_VERSION << "\n";
}

} // namespace pfind
