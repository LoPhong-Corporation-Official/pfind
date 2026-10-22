#ifndef PFIND_IGNORE_RULES_HPP
#define PFIND_IGNORE_RULES_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace pfind {

// A parsed .gitignore-style file.
//
// Supported syntax (same as git):
//   # comment, blank lines          ignored
//   pattern                         matches the name at any depth (no '/' in the pattern)
//   dir/                            matches directories only
//   /pattern, a/b                   anchored to the directory containing the ignore file
//   *  ?  [a-z]  [!a-z]             wildcards that never match '/'
//   **/x   x/**   a/**/b            "any number of directories"
//   !pattern                        re-includes something an earlier rule ignored
//   \#  \!  trailing "\ "           escapes
// Matching is case-sensitive. The last matching rule wins.
class IgnoreRules {
public:
    enum class Verdict { None, Ignored, Whitelisted };

    static IgnoreRules parse(std::string_view text);

    bool empty() const noexcept { return rules_.empty(); }
    std::size_t size() const noexcept { return rules_.size(); }

    // `relativePath` uses '/' separators and is relative to the directory of the ignore file.
    Verdict check(std::string_view relativePath, std::string_view name, bool isDirectory) const;

private:
    struct Range {
        char32_t lo;
        char32_t hi;
    };

    struct Token {
        enum class Kind { Literal, AnyOne, Star, DoubleStar, DoubleStarSlash, Class };
        Kind kind{Kind::Literal};
        char32_t ch{0};
        bool negate{false};
        std::vector<Range> ranges;
    };

    struct Rule {
        enum class Kind { Literal, Suffix, General };
        Kind kind{Kind::General};
        bool negated{false};
        bool dirOnly{false};
        bool anchored{false};
        std::string text;            // Literal / Suffix
        std::vector<Token> tokens;   // General
    };

    static std::vector<Token> tokenize(const std::u32string& pattern);
    static bool globMatch(const std::vector<Token>& tokens, std::string_view subject);

    std::vector<Rule> rules_;
};

} // namespace pfind

#endif
