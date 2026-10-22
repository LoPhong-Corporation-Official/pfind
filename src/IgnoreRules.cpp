#include "IgnoreRules.hpp"

#include "Utf8.hpp"

#include <algorithm>

namespace pfind {
namespace {

bool hasSpecial(std::string_view text, std::size_t from = 0) {
    return text.find_first_of("*?[\\", from) != std::string_view::npos;
}

// Recursive matcher with memoisation, so patterns with several '*' stay polynomial.
template <typename Token, typename Range>
struct GlobRun {
    const std::vector<Token>& tokens;
    const std::u32string& text;
    std::vector<signed char>& memo;
    std::size_t n;

    bool run(std::size_t ti, std::size_t xi) {
        signed char& slot = memo[ti * (n + 1) + xi];
        if (slot >= 0) {
            return slot != 0;
        }
        const bool result = compute(ti, xi);
        memo[ti * (n + 1) + xi] = result ? 1 : 0;
        return result;
    }

    bool compute(std::size_t ti, std::size_t xi) {
        if (ti == tokens.size()) {
            return xi == n;
        }

        const Token& token = tokens[ti];
        using Kind = typename Token::Kind;

        switch (token.kind) {
            case Kind::Literal:
                return xi < n && text[xi] == token.ch && run(ti + 1, xi + 1);

            case Kind::AnyOne:
                return xi < n && text[xi] != U'/' && run(ti + 1, xi + 1);

            case Kind::Class: {
                if (xi >= n || text[xi] == U'/') {
                    return false;
                }
                bool inside = false;
                for (const Range& r : token.ranges) {
                    if (text[xi] >= r.lo && text[xi] <= r.hi) {
                        inside = true;
                        break;
                    }
                }
                return inside != token.negate && run(ti + 1, xi + 1);
            }

            case Kind::Star:
                for (std::size_t k = xi;; ++k) {
                    if (run(ti + 1, k)) {
                        return true;
                    }
                    if (k >= n || text[k] == U'/') {
                        return false;
                    }
                }

            case Kind::DoubleStar:
                for (std::size_t k = xi; k <= n; ++k) {
                    if (run(ti + 1, k)) {
                        return true;
                    }
                }
                return false;

            case Kind::DoubleStarSlash:
                // Zero or more whole directories ("anything/").
                if (run(ti + 1, xi)) {
                    return true;
                }
                for (std::size_t k = xi; k < n; ++k) {
                    if (text[k] == U'/' && run(ti + 1, k + 1)) {
                        return true;
                    }
                }
                return false;
        }
        return false;
    }
};

} // namespace

std::vector<IgnoreRules::Token> IgnoreRules::tokenize(const std::u32string& p) {
    std::vector<Token> tokens;

    for (std::size_t i = 0; i < p.size(); ++i) {
        const char32_t c = p[i];
        Token token;

        if (c == U'*') {
            std::size_t j = i;
            while (j < p.size() && p[j] == U'*') {
                ++j;
            }

            const bool doubleStar = (j - i) >= 2;
            const bool atSegmentStart = (i == 0 || p[i - 1] == U'/');

            if (doubleStar && atSegmentStart && j < p.size() && p[j] == U'/') {
                token.kind = Token::Kind::DoubleStarSlash;  // "**/"
                i = j;                                       // also skips the '/'
            } else if (doubleStar && atSegmentStart && j == p.size()) {
                token.kind = Token::Kind::DoubleStar;        // trailing "/**"
                i = j - 1;
            } else {
                token.kind = Token::Kind::Star;
                i = j - 1;
            }
        } else if (c == U'?') {
            token.kind = Token::Kind::AnyOne;
        } else if (c == U'\\' && i + 1 < p.size()) {
            token.ch = p[++i];
        } else if (c == U'[') {
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
                token.ch = c;  // unterminated: literal '['
            }
        } else {
            token.ch = c;
        }

        tokens.push_back(std::move(token));
    }

    return tokens;
}

bool IgnoreRules::globMatch(const std::vector<Token>& tokens, std::string_view subject) {
    thread_local std::u32string text;
    thread_local std::vector<signed char> memo;

    utf8::normalizeInto(subject, false, false, text);
    memo.assign((tokens.size() + 1) * (text.size() + 1), static_cast<signed char>(-1));

    GlobRun<Token, Range> state{tokens, text, memo, text.size()};
    return state.run(0, 0);
}

IgnoreRules IgnoreRules::parse(std::string_view text) {
    IgnoreRules result;

    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") {
        text.remove_prefix(3);  // UTF-8 BOM
    }

    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t newline = text.find('\n', start);
        std::string line(text.substr(start, newline == std::string_view::npos ? std::string_view::npos
                                                                               : newline - start));
        start = (newline == std::string_view::npos) ? text.size() + 1 : newline + 1;

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Trailing spaces are ignored unless escaped with a backslash.
        while (!line.empty() && line.back() == ' ') {
            if (line.size() >= 2 && line[line.size() - 2] == '\\') {
                break;
            }
            line.pop_back();
        }

        if (line.empty() || line[0] == '#') {
            continue;
        }

        Rule rule;

        if (line[0] == '!') {
            rule.negated = true;
            line.erase(0, 1);
        }

        if (!line.empty() && line.back() == '/') {
            rule.dirOnly = true;
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        rule.anchored = line.find('/') != std::string::npos;
        if (line[0] == '/') {
            line.erase(0, 1);
        }
        if (line.empty()) {
            continue;
        }

        if (!hasSpecial(line)) {
            rule.kind = Rule::Kind::Literal;
            rule.text = line;
        } else if (!rule.anchored && line[0] == '*' && line.size() > 1 && !hasSpecial(line, 1)) {
            rule.kind = Rule::Kind::Suffix;   // the very common "*.ext"
            rule.text = line.substr(1);
        } else {
            rule.kind = Rule::Kind::General;
            rule.tokens = tokenize(utf8::decode(line));
        }

        result.rules_.push_back(std::move(rule));
    }

    return result;
}

IgnoreRules::Verdict IgnoreRules::check(std::string_view relativePath,
                                        std::string_view name,
                                        bool isDirectory) const {
    for (auto it = rules_.rbegin(); it != rules_.rend(); ++it) {
        const Rule& rule = *it;
        if (rule.dirOnly && !isDirectory) {
            continue;
        }

        const std::string_view subject = rule.anchored ? relativePath : name;
        bool hit = false;

        switch (rule.kind) {
            case Rule::Kind::Literal:
                hit = subject == rule.text;
                break;
            case Rule::Kind::Suffix:
                hit = subject.size() >= rule.text.size() &&
                      subject.substr(subject.size() - rule.text.size()) == rule.text;
                break;
            case Rule::Kind::General:
                hit = globMatch(rule.tokens, subject);
                break;
        }

        if (hit) {
            return rule.negated ? Verdict::Whitelisted : Verdict::Ignored;
        }
    }

    return Verdict::None;
}

} // namespace pfind
