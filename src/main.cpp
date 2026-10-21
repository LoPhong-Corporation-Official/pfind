#include "CLIParser.hpp"
#include "Format.hpp"
#include "SearchEngine.hpp"
#include "Utf8.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
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

std::string describe(const pfind::FileInfo& file, bool details) {
    std::string line = pfind::utf8::fromPath(file.path);

    if (details) {
        line += "  [";
        line += file.isDirectory ? "<dir>" : pfind::formatSize(file.size);
        line += "]  ";
        line += pfind::formatLocalTime(file.lastWriteTime);
    }

    return line;
}

std::string toJson(const pfind::FileInfo& file) {
    std::string json = "{\"path\":\"" + pfind::jsonEscape(pfind::utf8::fromPath(file.path)) + "\"";
    json += ",\"type\":\"";
    json += file.isDirectory ? "dir" : "file";
    json += "\",\"size\":" + std::to_string(file.size);
    json += ",\"modified\":\"" + pfind::formatIsoUtc(file.lastWriteTime) + "\"}";
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
        std::cerr << "----------------------------------------\n";
    }

    // Unsorted output is streamed immediately; sorted output needs all matches first.
    const bool collect = output.sort != pfind::SortKey::None;
    std::vector<pfind::FileInfo> collected;

    auto emit = [&](const pfind::FileInfo& file) {
        if (output.countOnly) {
            return;
        }
        if (output.json) {
            std::cout << toJson(file) << '\n';
        } else if (output.null) {
            std::cout << pfind::utf8::fromPath(file.path) << '\0';
        } else {
            std::cout << describe(file, options.showDetails) << '\n';
        }
    };

    // Sorting by size/time and JSON output need metadata even without --details.
    options.collectMetadata = options.showDetails || output.json ||
                              output.sort == pfind::SortKey::Size ||
                              output.sort == pfind::SortKey::Time;

    pfind::SearchEngine engine(options);
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
        for (const auto& file : collected) {
            emit(file);
        }
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    if (output.countOnly) {
        std::cout << count << '\n';
    }

    if (!output.quiet) {
        if (!output.countOnly) {
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

    if (engine.rootFailed()) {
        return 2;
    }
    return count > 0 ? 0 : 1;
}
