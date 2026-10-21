#ifndef PFIND_WILDCARD_HPP
#define PFIND_WILDCARD_HPP

#include <string>
#include <string_view>
#include <vector>

namespace pfind {

struct MatchOptions {
    bool caseSensitive{false};
    bool ignoreAccents{false};
};

// A compiled wildcard pattern working on UTF-8 text.
//
// Syntax:
//   *        zero or more characters
//   ?        exactly one character (a full Unicode character, not a byte)
//   [abc]    any one of the listed characters
//   [a-z]    any character in the range
//   [!abc]   any character NOT listed ([^abc] is accepted too)
//   \x       the literal character x
class Matcher {
public:
    explicit Matcher(std::string_view pattern, MatchOptions options = {});

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
};

// Convenience wrapper kept for API compatibility.
bool wildcardMatch(std::string_view text,
                   std::string_view pattern,
                   bool caseSensitive = false);

} // namespace pfind

#endif
