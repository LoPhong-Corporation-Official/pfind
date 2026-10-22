#ifndef PFIND_WILDCARD_HPP
#define PFIND_WILDCARD_HPP

#include <cstddef>
#include <cstdint>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
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

    // Byte ranges [begin, end) of every hit inside `text`, for highlighting. Works for literal
    // matchers and regular expressions (not for regexes combined with ignoreAccents); wildcard
    // patterns return nothing. At most 100 ranges are returned.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> findAll(std::string_view text,
                                                                  std::size_t limit = 100) const;

    // Returns `text` with every hit replaced. Literal matchers insert `replacement` verbatim;
    // regular expressions expand $1, $2, $& and $$ in it. `count` receives the number of
    // replacements. Regex lines longer than 64 KB and regex + ignoreAccents are left unchanged.
    std::string replaceAll(std::string_view text, std::string_view replacement, std::size_t& count) const;

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
