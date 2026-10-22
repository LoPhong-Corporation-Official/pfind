#ifndef PFIND_CLI_PARSER_HPP
#define PFIND_CLI_PARSER_HPP

#include "SearchOptions.hpp"

#include <optional>
#include <string>
#include <vector>

#ifndef PFIND_VERSION
#define PFIND_VERSION "1.1.0"
#endif

namespace pfind {

enum class SortKey { None, Name, Size, Time };
enum class ColorMode { Auto, Always, Never };

struct OutputOptions {
    SortKey sort{SortKey::None};
    bool reverse{false};
    bool json{false};       // one JSON object per line
    bool null{false};       // NUL-separated paths (for xargs -0)
    bool countOnly{false};
    bool quiet{false};      // no summary / warnings header
    bool verbose{false};    // print the effective search settings
    ColorMode color{ColorMode::Auto};
};

struct ReplaceOptions {
    std::optional<std::string> text;   // --replace
    bool apply{false};                 // --apply (otherwise: preview only)
    bool backup{false};                // --backup
};

struct CLIResult {
    bool showHelp{false};
    bool showVersion{false};
    bool valid{true};
    std::string error;
    SearchOptions options;
    OutputOptions output;
    ReplaceOptions replace;
    std::string completionShell;   // --completion <shell>
};

class CLIParser {
public:
    // args[0] is the program name, exactly like argv.
    static CLIResult parse(const std::vector<std::string>& args);
    static CLIResult parse(int argc, char* argv[]);

    static void printHelp(const char* programName);
    static void printVersion();
};

} // namespace pfind

#endif
