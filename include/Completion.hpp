#ifndef PFIND_COMPLETION_HPP
#define PFIND_COMPLETION_HPP

#include <string>
#include <string_view>
#include <vector>

namespace pfind {

// Description of one command-line option. The completion scripts are generated from this table,
// and the tests check it against the real parser and the --help text so they cannot drift apart.
struct OptionSpec {
    std::string_view shortName;    // "-p", or "" if there is none
    std::string_view longName;     // "--pattern"
    std::string_view valueName;    // "pattern"; empty for flags
    std::string_view choices;      // "file dir all" (space separated); empty = free text
    std::string_view description;
    std::string_view sample;       // a valid example value (used by tests)
    bool repeatable;               // may be given several times
};

const std::vector<OptionSpec>& optionSpecs();

bool isCompletionShell(std::string_view shell);

// Returns the completion script for "bash", "zsh" or "powershell" (empty for anything else).
std::string completionScript(std::string_view shell);

} // namespace pfind

#endif
