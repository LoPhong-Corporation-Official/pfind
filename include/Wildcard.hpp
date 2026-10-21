#ifndef PFIND_WILDCARD_HPP
#define PFIND_WILDCARD_HPP

#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace pfind {

struct MatchOptions {
    bool caseSensitive{false};
    bool ignoreAccents{false};
    bool regex{false};  // treat the pattern as an ECMAScript regular expression (search semantics)
};

// A compiled wildcard pattern working on UTF-8 text.
//
// With MatchOptions::regex the pattern is a regular expression instead (std::regex, ECMAScript,
// found anywhere in the text; case-insensitivity is ASCII-only in that mode).
//
// Wildcard syntax:
//   *        zero or more characters
//   ?        exactly one character (a full Unicode character, not a byte)
//   [abc]    any one of the listed characters
//   [a-z]    any character in the range
//   [!abc]   any character NOT listed ([^abc] is accepted too)
//   \x       the literal character x
class Matcher {
public:
    // Throws std::invalid_argument if a regular expression is invalid.
    explicit Matcher(std::string_view pattern, MatchOptions options = {});

    // Matches any text that CONTAINS `text` literally (wildcard characters in it are escaped).
    static Matcher literal(std::string_view text, MatchOptions options = {});

    bool matches(std::string_view text) const;

private:
    struct Range {
        char32_t lo;
        char32_t hi;
    };

    struct Token {
        enum class Kind { Literal, AnyOne, AnyMany, Class };
        Kind kind{Kind::Literal};
        char32_t ch{0};
        bool negate{false};
        std::vector<Range> ranges;
    };

    static bool matchesSingle(const Token& token, char32_t c);

    MatchOptions options_;
    std::vector<Token> tokens_;
    std::regex regex_;

    // Fast path for Matcher::literal() with a pure-ASCII needle (the common grep case).
    bool useNeedle_{false};
    std::string needle_;

    // Literal (substring) matcher: normalised needle searched directly, no wildcard machinery.
    bool useLiteral_{false};
    std::u32string literal_;
};

// Convenience wrapper kept for API compatibility.
bool wildcardMatch(std::string_view text,
                   std::string_view pattern,
                   bool caseSensitive = false);

} // namespace pfind

#endif
