#include "Wildcard.hpp"

#include "Utf8.hpp"

#include <stdexcept>

namespace pfind {
namespace {

bool isAscii(std::string_view text) {
    unsigned char accumulated = 0;
    for (const char c : text) {
        accumulated |= static_cast<unsigned char>(c);
    }
    return accumulated < 0x80;
}

unsigned char lowerAscii(char c) {
    const auto u = static_cast<unsigned char>(c);
    return (u >= 'A' && u <= 'Z') ? static_cast<unsigned char>(u + 32) : u;
}

// `needle` is already lower-case when !caseSensitive.
bool asciiContains(std::string_view text, const std::string& needle, bool caseSensitive) {
    if (needle.size() > text.size()) {
        return false;
    }
    if (caseSensitive) {
        return text.find(needle) != std::string_view::npos;
    }

    const auto first = static_cast<unsigned char>(needle[0]);
    const std::size_t last = text.size() - needle.size();
    for (std::size_t i = 0; i <= last; ++i) {
        if (lowerAscii(text[i]) != first) {
            continue;
        }
        std::size_t k = 1;
        while (k < needle.size() && lowerAscii(text[i + k]) == static_cast<unsigned char>(needle[k])) {
            ++k;
        }
        if (k == needle.size()) {
            return true;
        }
    }
    return false;
}

} // namespace

Matcher::Matcher(std::string_view pattern, MatchOptions options)
    : options_(options) {
    if (options.regex) {
        auto flags = std::regex::ECMAScript | std::regex::optimize;
        if (!options.caseSensitive) {
            flags |= std::regex::icase;
        }
        const std::string source = options.ignoreAccents
            ? utf8::encode(utf8::normalize(pattern, false, true))
            : std::string(pattern);
        try {
            regex_ = std::regex(source, flags);
        } catch (const std::regex_error& error) {
            throw std::invalid_argument("Invalid regular expression '" + std::string(pattern) +
                                        "': " + error.what());
        }
        return;
    }

    const std::u32string p =
        utf8::normalize(pattern, !options.caseSensitive, options.ignoreAccents);

    for (std::size_t i = 0; i < p.size(); ++i) {
        const char32_t c = p[i];
        Token token;

        if (c == U'*') {
            if (!tokens_.empty() && tokens_.back().kind == Token::Kind::AnyMany) {
                continue; // collapse "**"
            }
            token.kind = Token::Kind::AnyMany;
        } else if (c == U'?') {
            token.kind = Token::Kind::AnyOne;
        } else if (c == U'\\' && i + 1 < p.size()) {
            token.kind = Token::Kind::Literal;
            token.ch = p[++i];
        } else if (c == U'[') {
            // Find the closing bracket; if missing, '[' is a literal.
            std::size_t j = i + 1;
            bool negate = false;
            if (j < p.size() && (p[j] == U'!' || p[j] == U'^')) {
                negate = true;
                ++j;
            }

            std::vector<Range> ranges;
            bool closed = false;
            bool first = true;
            while (j < p.size()) {
                char32_t lo = p[j];
                if (lo == U']' && !first) {
                    closed = true;
                    break;
                }
                first = false;

                if (lo == U'\\' && j + 1 < p.size()) {
                    lo = p[++j];
                }

                char32_t hi = lo;
                if (j + 2 < p.size() && p[j + 1] == U'-' && p[j + 2] != U']') {
                    hi = p[j + 2];
                    if (hi == U'\\' && j + 3 < p.size()) {
                        hi = p[j + 3];
                        ++j;
                    }
                    j += 2;
                }
                if (hi < lo) {
                    std::swap(lo, hi);
                }

                ranges.push_back({lo, hi});
                ++j;
            }

            if (closed) {
                token.kind = Token::Kind::Class;
                token.negate = negate;
                token.ranges = std::move(ranges);
                i = j;
            } else {
                token.kind = Token::Kind::Literal;
                token.ch = c;
            }
        } else {
            token.kind = Token::Kind::Literal;
            token.ch = c;
        }

        tokens_.push_back(std::move(token));
    }
}

bool Matcher::matchesSingle(const Token& token, char32_t c) {
    switch (token.kind) {
        case Token::Kind::Literal:
            return token.ch == c;
        case Token::Kind::AnyOne:
            return true;
        case Token::Kind::Class: {
            bool inside = false;
            for (const Range& r : token.ranges) {
                if (c >= r.lo && c <= r.hi) {
                    inside = true;
                    break;
                }
            }
            return inside != token.negate;
        }
        case Token::Kind::AnyMany:
            break;
    }
    return false;
}

Matcher Matcher::literal(std::string_view text, MatchOptions options) {
    options.regex = false;
    std::string pattern = "*";
    for (const char c : text) {
        if (c == '*' || c == '?' || c == '[' || c == '\\') {
            pattern += '\\';
        }
        pattern += c;
    }
    pattern += '*';

    Matcher matcher(pattern, options);

    matcher.useLiteral_ = !text.empty();
    matcher.literal_ = utf8::normalize(text, !options.caseSensitive, options.ignoreAccents);

    bool ascii = !text.empty() && isAscii(text);
    if (ascii) {
        matcher.useNeedle_ = true;
        matcher.needle_.reserve(text.size());
        for (const char c : text) {
            matcher.needle_ += options.caseSensitive ? c : static_cast<char>(lowerAscii(c));
        }
    }
    return matcher;
}

bool Matcher::matches(std::string_view text) const {
    if (options_.regex) {
        // std::regex can recurse deeply on huge inputs; cap what we feed it.
        constexpr std::size_t kMaxRegexInput = 64 * 1024;
        const std::string_view input = text.substr(0, kMaxRegexInput);
        if (options_.ignoreAccents) {
            const std::string subject = utf8::encode(utf8::normalize(input, false, true));
            return std::regex_search(subject.data(), subject.data() + subject.size(), regex_);
        }
        return std::regex_search(input.data(), input.data() + input.size(), regex_);
    }

    // Pure-ASCII text can never differ because of accents, so a plain byte search is exact.
    if (useNeedle_ && isAscii(text)) {
        return asciiContains(text, needle_, options_.caseSensitive);
    }

    thread_local std::u32string buffer;
    utf8::normalizeInto(text, !options_.caseSensitive, options_.ignoreAccents, buffer);
    const std::u32string& t = buffer;

    if (useLiteral_) {
        return t.find(literal_) != std::u32string::npos;
    }

    // Iterative matching with backtracking only to the latest '*': O(n*m) worst case.
    std::size_t textPos = 0;
    std::size_t tokenPos = 0;
    std::size_t starToken = std::u32string::npos;
    std::size_t starText = 0;

    while (textPos < t.size()) {
        if (tokenPos < tokens_.size()) {
            const Token& token = tokens_[tokenPos];
            if (token.kind == Token::Kind::AnyMany) {
                starToken = tokenPos++;
                starText = textPos;
                continue;
            }
            if (matchesSingle(token, t[textPos])) {
                ++textPos;
                ++tokenPos;
                continue;
            }
        }

        if (starToken != std::u32string::npos) {
            tokenPos = starToken + 1;
            textPos = ++starText;
            continue;
        }

        return false;
    }

    while (tokenPos < tokens_.size() && tokens_[tokenPos].kind == Token::Kind::AnyMany) {
        ++tokenPos;
    }

    return tokenPos == tokens_.size();
}

bool wildcardMatch(std::string_view text, std::string_view pattern, bool caseSensitive) {
    MatchOptions options;
    options.caseSensitive = caseSensitive;
    return Matcher(pattern, options).matches(text);
}

} // namespace pfind
