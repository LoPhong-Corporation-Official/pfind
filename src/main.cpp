#include "CLIParser.hpp"
#include "Completion.hpp"
#include "Replace.hpp"
#include "Format.hpp"
#include "SearchEngine.hpp"
#include "Utf8.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#define PFIND_ISATTY(fd) _isatty(fd)
#define PFIND_FILENO(f) _fileno(f)
#else
#include <unistd.h>
#define PFIND_ISATTY(fd) isatty(fd)
#define PFIND_FILENO(f) fileno(f)
#endif

namespace {

#ifdef _WIN32
// The narrow argv uses the ANSI code page; fetch the real UTF-16 arguments instead.
std::vector<std::string> getUtf8Arguments() {
    std::vector<std::string> args;
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr) {
        return args;
    }

    for (int i = 0; i < count; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(size > 0 ? static_cast<std::size_t>(size - 1) : 0, '\0');
        if (size > 1) {
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, utf8.data(), size, nullptr, nullptr);
        }
        args.push_back(std::move(utf8));
    }

    LocalFree(wide);
    return args;
}
#endif

// ANSI escape sequences; all empty when colour is off.
struct Palette {
    const char* path = "";
    const char* directory = "";
    const char* lineNumber = "";
    const char* separator = "";
    const char* dim = "";
    const char* match = "";
    const char* removed = "";
    const char* added = "";
    const char* reset = "";
};

// Reads an environment variable; empty if it is not set (std::getenv is flagged as unsafe by MSVC).
std::string environmentVariable(const char* name) {
#ifdef _WIN32
    // Win32 API: no MSVC deprecation warning and available in MSVC and MinGW alike.
    const DWORD size = GetEnvironmentVariableA(name, nullptr, 0);   // includes the terminating NUL
    if (size == 0) {
        return {};
    }
    std::string result(size, '\0');
    const DWORD written = GetEnvironmentVariableA(name, result.data(), size);
    result.resize(written);
    return result;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

bool shouldUseColor(pfind::ColorMode mode) {
    if (mode == pfind::ColorMode::Never) {
        return false;
    }
    if (mode == pfind::ColorMode::Always) {
        return true;
    }

    if (!environmentVariable("NO_COLOR").empty()) {
        return false;
    }
    if (environmentVariable("TERM") == "dumb") {
        return false;
    }
    return PFIND_ISATTY(PFIND_FILENO(stdout)) != 0;
}

Palette makePalette(bool enabled) {
    Palette palette;
    if (enabled) {
        palette.path = "\x1b[35m";        // magenta
        palette.directory = "\x1b[1;34m"; // bold blue
        palette.lineNumber = "\x1b[32m";  // green
        palette.separator = "\x1b[36m";   // cyan
        palette.dim = "\x1b[2m";
        palette.match = "\x1b[1;31m";   // bold red
        palette.removed = "\x1b[31m";
        palette.added = "\x1b[32m";
        palette.reset = "\x1b[0m";
    }
    return palette;
}

// Text with the given byte ranges wrapped in the match colour.
std::string highlight(const std::string& text,
                      const std::vector<std::pair<std::uint32_t, std::uint32_t>>& ranges,
                      const Palette& colors) {
    if (ranges.empty() || *colors.match == '\0') {
        return text;
    }

    std::string out;
    std::size_t position = 0;
    for (const auto& [from, to] : ranges) {
        if (from < position || to > text.size() || from >= to) {
            continue;   // overlapping or out of range (e.g. cut off by truncation)
        }
        out.append(text, position, from - position);
        out += colors.match;
        out.append(text, from, to - from);
        out += colors.reset;
        position = to;
    }
    out.append(text, position, std::string::npos);
    return out;
}

std::string describe(const pfind::FileInfo& file, bool details, const Palette& colors) {
    std::string line = file.isDirectory ? colors.directory : colors.path;
    line += pfind::utf8::fromPath(file.path);
    line += colors.reset;

    if (details) {
        line += colors.dim;
        line += "  [";
        line += file.isDirectory ? "<dir>" : pfind::formatSize(file.size);
        line += "]  ";
        line += pfind::formatLocalTime(file.lastWriteTime);
        line += colors.reset;
    }

    return line;
}

std::string toJson(const pfind::FileInfo& file) {
    std::string json = "{\"path\":\"" + pfind::jsonEscape(pfind::utf8::fromPath(file.path)) + "\"";
    json += ",\"type\":\"";
    json += file.isDirectory ? "dir" : "file";
    json += "\",\"size\":" + std::to_string(file.size);
    json += ",\"modified\":\"" + pfind::formatIsoUtc(file.lastWriteTime) + "\"";
    if (!file.matches.empty()) {
        json += ",\"matches\":[";
        for (std::size_t i = 0; i < file.matches.size(); ++i) {
            if (i > 0) json += ',';
            json += "{\"line\":" + std::to_string(file.matches[i].line) +
                    ",\"text\":\"" + pfind::jsonEscape(file.matches[i].text) + "\"" +
                    (file.matches[i].context ? ",\"context\":true" : "") + "}";
        }
        json += ']';
    }
    json += '}';
    return json;
}

void sortResults(std::vector<pfind::FileInfo>& results, const pfind::OutputOptions& output) {
    auto nameKey = [](const pfind::FileInfo& f) {
        return pfind::utf8::normalize(pfind::utf8::fromPath(f.path), true, false);
    };

    switch (output.sort) {
        case pfind::SortKey::Name:
            std::stable_sort(results.begin(), results.end(),
                             [&](const auto& a, const auto& b) { return nameKey(a) < nameKey(b); });
            break;
        case pfind::SortKey::Size:
            std::stable_sort(results.begin(), results.end(),
                             [](const auto& a, const auto& b) { return a.size < b.size; });
            break;
        case pfind::SortKey::Time:
            std::stable_sort(results.begin(), results.end(),
                             [](const auto& a, const auto& b) { return a.lastWriteTime < b.lastWriteTime; });
            break;
        case pfind::SortKey::None:
            break;
    }

    if (output.reverse) {
        std::reverse(results.begin(), results.end());
    }
}

} // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args = getUtf8Arguments();
    if (args.empty()) {
        for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
    }
    const pfind::CLIResult cli = pfind::CLIParser::parse(args);
#else
    const pfind::CLIResult cli = pfind::CLIParser::parse(argc, argv);
#endif

    if (cli.showHelp) {
        pfind::CLIParser::printHelp(argc > 0 ? argv[0] : "pfind");
        return 0;
    }

    if (cli.showVersion) {
        pfind::CLIParser::printVersion();
        return 0;
    }

    if (!cli.completionShell.empty()) {
        std::cout << pfind::completionScript(cli.completionShell);
        return 0;
    }

    if (!cli.valid) {
        std::cerr << "Error: " << cli.error << '\n';
        std::cerr << "Use --help for usage information.\n";
        return 2;
    }

    std::error_code absoluteEc;
    const auto absoluteRoot = std::filesystem::absolute(cli.options.root, absoluteEc);
    if (absoluteEc) {
        std::cerr << "Error: invalid search path: " << absoluteEc.message() << '\n';
        return 2;
    }

    pfind::SearchOptions options = cli.options;
    options.root = absoluteRoot;
    const pfind::OutputOptions& output = cli.output;

    if (output.verbose) {
        std::cerr << "Searching: " << pfind::utf8::fromPath(options.root) << '\n';
        for (const auto& p : options.patterns) std::cerr << "Pattern:   " << p << '\n';
        for (const auto& x : options.excludes) std::cerr << "Exclude:   " << x << '\n';
        if (options.contentPattern) std::cerr << "Contains:  " << *options.contentPattern << '\n';
        std::cerr << "----------------------------------------\n";
    }

    // Unsorted output is streamed immediately; sorted output needs all matches first.
    const bool replaceMode = cli.replace.text.has_value();
    if (replaceMode) {
        options.filesWithMatches = true;   // we only need to know WHICH files contain a match
    }
    const bool collect = output.sort != pfind::SortKey::None || replaceMode;
    std::vector<pfind::FileInfo> collected;

    bool printedGroup = false;

    const bool plainOutput = !output.json && !output.null && !output.countOnly;
    bool useColor = plainOutput && shouldUseColor(output.color);
#ifdef _WIN32
    if (useColor) {
        // Enable ANSI sequences on the Windows console (Windows 10+).
        HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (handle == INVALID_HANDLE_VALUE || !GetConsoleMode(handle, &mode) ||
            !SetConsoleMode(handle, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */)) {
            useColor = output.color == pfind::ColorMode::Always;  // redirected output: honour "always"
        }
    }
#endif
    const Palette colors = makePalette(useColor);
    options.highlightMatches = useColor && options.contentPattern.has_value();
    auto emit = [&](const pfind::FileInfo& file) {
        if (output.countOnly) {
            return;
        }
        if (output.json) {
            std::cout << toJson(file) << '\n';
        } else if (output.null) {
            std::cout << pfind::utf8::fromPath(file.path) << '\0';
        } else if (!file.matches.empty() && !options.filesWithMatches) {
            const std::string path = pfind::utf8::fromPath(file.path);
            const bool withContext = options.beforeContext > 0 || options.afterContext > 0;

            if (withContext && printedGroup) {
                std::cout << colors.separator << "--" << colors.reset << '\n';  // separates files
            }
            printedGroup = true;

            std::size_t previousLine = 0;
            for (const auto& match : file.matches) {
                if (withContext && previousLine != 0 && match.line != previousLine + 1) {
                    std::cout << colors.separator << "--" << colors.reset << '\n';  // gap between groups
                }
                previousLine = match.line;

                const char separator = match.context ? '-' : ':';
                std::cout << colors.path << path << colors.reset
                          << colors.separator << separator << colors.reset
                          << colors.lineNumber << match.line << colors.reset
                          << colors.separator << separator << colors.reset << ' '
                          << (match.context ? colors.dim : "")
                          << (match.context ? match.text : highlight(match.text, match.ranges, colors))
                          << (match.context ? colors.reset : "") << '\n';
            }
        } else {
            std::cout << describe(file, options.showDetails, colors) << '\n';
        }
    };

    // Sorting by size/time and JSON output need metadata even without --details.
    options.collectMetadata = options.showDetails || output.json ||
                              output.sort == pfind::SortKey::Size ||
                              output.sort == pfind::SortKey::Time;

    std::optional<pfind::SearchEngine> engineStorage;
    try {
        engineStorage.emplace(options);
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 2;
    }
    pfind::SearchEngine& engine = *engineStorage;
    const auto start = std::chrono::steady_clock::now();

    const std::size_t count = engine.search([&](const pfind::FileInfo& file) {
        if (collect && !output.countOnly) {
            collected.push_back(file);
        } else {
            emit(file);
        }
    });

    if (collect && !output.countOnly) {
        sortResults(collected, output);
    }
    if (collect && !output.countOnly && !replaceMode) {
        for (const auto& file : collected) {
            emit(file);
        }
    }

    // ---- --replace: preview or apply ----
    std::size_t replacedTotal = 0;
    std::size_t filesChanged = 0;
    bool replaceFailed = false;
    std::vector<std::string> replaceErrors;

    if (replaceMode && engine.contentMatcher() != nullptr) {
        const bool apply = cli.replace.apply;
        for (const auto& file : collected) {
            const pfind::ReplaceResult result = pfind::replaceInFile(
                file.path, *engine.contentMatcher(), *cli.replace.text, apply, cli.replace.backup);

            if (!result.error.empty()) {
                replaceFailed = true;
                replaceErrors.push_back(result.error + ": " + pfind::utf8::fromPath(file.path));
                continue;
            }
            if (result.changes.empty()) {
                continue;
            }

            ++filesChanged;
            replacedTotal += result.replacements;

            std::cout << colors.path << pfind::utf8::fromPath(file.path) << colors.reset << "  ("
                      << result.replacements << (result.replacements == 1 ? " replacement" : " replacements")
                      << (apply ? ", written" : "") << ")\n";

            if (!apply) {
                for (const auto& change : result.changes) {
                    std::cout << colors.lineNumber << "  " << change.line << colors.reset << ' '
                              << colors.removed << "- " << change.before << colors.reset << '\n'
                              << colors.lineNumber << "  " << change.line << colors.reset << ' '
                              << colors.added << "+ " << change.after << colors.reset << '\n';
                }
            }
        }
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    if (output.countOnly) {
        std::cout << count << '\n';
    }

    if (!output.quiet) {
        if (replaceMode) {
            std::cerr << replacedTotal << (replacedTotal == 1 ? " replacement" : " replacements")
                      << " in " << filesChanged << (filesChanged == 1 ? " file" : " files")
                      << (cli.replace.apply ? " written" : " (preview only - nothing was written; add --apply)")
                      << ", " << elapsed.count() << " ms.\n";
            for (const auto& error : replaceErrors) {
                std::cerr << "  ! " << error << '\n';
            }
        } else if (!output.countOnly) {
            std::cerr << count << " match" << (count == 1 ? "" : "es")
                      << " found in " << elapsed.count() << " ms.\n";
        }

        if (!engine.errors().empty()) {
            std::cerr << "\nWarnings (" << engine.errors().size() << "):\n";
            constexpr std::size_t kMaxShown = 50;
            std::size_t shown = 0;
            for (const auto& error : engine.errors()) {
                if (shown++ == kMaxShown) {
                    std::cerr << "  ... and " << (engine.errors().size() - kMaxShown) << " more\n";
                    break;
                }
                std::cerr << "  - " << error << '\n';
            }
        }
    }

    if (engine.rootFailed() || replaceFailed) {
        return 2;
    }
    if (replaceMode) {
        return replacedTotal > 0 ? 0 : 1;
    }
    return count > 0 ? 0 : 1;
}
