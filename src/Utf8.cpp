#include "Utf8.hpp"

#include <algorithm>
#include <array>
#include <iterator>

namespace pfind::utf8 {
namespace {

struct AccentEntry {
    char32_t from;
    char32_t to;
};

constexpr AccentEntry kAccentTable[] = {
#include "AccentTable.inc"
};

constexpr char32_t kEscapeBase = 0xDC00;

} // namespace

namespace {

// Decodes one code point starting at text[i]; returns the number of bytes consumed (>= 1).
std::size_t decodeOne(std::string_view text, std::size_t i, char32_t& cp) {
    const auto b0 = static_cast<unsigned char>(text[i]);
    if (b0 < 0x80) {
        cp = b0;
        return 1;
    }

    std::size_t length = 0;
    char32_t value = 0;
    char32_t minimum = 0;

    if ((b0 & 0xE0) == 0xC0) {
        length = 2; value = b0 & 0x1F; minimum = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        length = 3; value = b0 & 0x0F; minimum = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        length = 4; value = b0 & 0x07; minimum = 0x10000;
    }

    bool ok = length != 0 && i + length <= text.size();
    for (std::size_t k = 1; ok && k < length; ++k) {
        const auto b = static_cast<unsigned char>(text[i + k]);
        if ((b & 0xC0) != 0x80) {
            ok = false;
        } else {
            value = (value << 6) | (b & 0x3F);
        }
    }

    if (ok && value >= minimum && value <= 0x10FFFF && !(value >= 0xD800 && value <= 0xDFFF)) {
        cp = value;
        return length;
    }

    cp = kEscapeBase + b0;  // invalid byte: keep it, losslessly
    return 1;
}

} // namespace

std::u32string decode(std::string_view text) {
    std::u32string out;
    out.reserve(text.size());

    for (std::size_t i = 0; i < text.size();) {
        char32_t cp = 0;
        i += decodeOne(text, i, cp);
        out.push_back(cp);
    }

    return out;
}

std::string encode(std::u32string_view text) {
    std::string out;
    out.reserve(text.size());

    for (const char32_t cp : text) {
        if (cp >= kEscapeBase + 0x80 && cp <= kEscapeBase + 0xFF) {
            out.push_back(static_cast<char>(cp - kEscapeBase));
        } else if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    return out;
}

std::filesystem::path toPath(std::string_view utf8) {
    const std::u8string u8(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(u8);
}

std::string fromPath(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::string fromPathGeneric(const std::filesystem::path& path) {
    const std::u8string u8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

char32_t foldCase(char32_t cp) noexcept {
    if (cp < 0x80) {
        return (cp >= U'A' && cp <= U'Z') ? cp + 0x20 : cp;
    }

    // Latin-1 Supplement.
    if (cp >= 0x00C0 && cp <= 0x00DE && cp != 0x00D7) return cp + 0x20;

    // Latin Extended-A.
    if (cp >= 0x0100 && cp <= 0x0137) return (cp == 0x0130) ? cp : ((cp % 2 == 0) ? cp + 1 : cp);
    if (cp >= 0x0139 && cp <= 0x0148) return (cp % 2 == 1) ? cp + 1 : cp;
    if (cp >= 0x014A && cp <= 0x0177) return (cp % 2 == 0) ? cp + 1 : cp;
    if (cp == 0x0178) return 0x00FF;
    if (cp >= 0x0179 && cp <= 0x017E) return (cp % 2 == 1) ? cp + 1 : cp;

    // Vietnamese horn letters.
    if (cp == 0x01A0 || cp == 0x01AF) return cp + 1;

    // Latin Extended Additional (all precomposed Vietnamese letters).
    if (cp >= 0x1EA0 && cp <= 0x1EF9) return (cp % 2 == 0) ? cp + 1 : cp;

    // Greek and Cyrillic.
    if (cp >= 0x0391 && cp <= 0x03A9 && cp != 0x03A2) return cp + 0x20;
    if (cp >= 0x0410 && cp <= 0x042F) return cp + 0x20;
    if (cp >= 0x0400 && cp <= 0x040F) return cp + 0x50;

    return cp;
}

bool isCombiningMark(char32_t cp) noexcept {
    return cp >= 0x0300 && cp <= 0x036F;
}

namespace {

// Direct lookup tables for the two ranges the accent table covers (built once, thread-safe).
struct AccentLookup {
    static constexpr char32_t kLowStart = 0x00C0;
    static constexpr char32_t kLowEnd = 0x024F;
    static constexpr char32_t kHighStart = 0x1E00;
    static constexpr char32_t kHighEnd = 0x1EFF;

    std::array<char32_t, kLowEnd - kLowStart + 1> low{};
    std::array<char32_t, kHighEnd - kHighStart + 1> high{};

    AccentLookup() {
        for (std::size_t i = 0; i < low.size(); ++i) {
            low[i] = static_cast<char32_t>(kLowStart + i);
        }
        for (std::size_t i = 0; i < high.size(); ++i) {
            high[i] = static_cast<char32_t>(kHighStart + i);
        }
        for (const AccentEntry& entry : kAccentTable) {
            if (entry.from >= kLowStart && entry.from <= kLowEnd) {
                low[entry.from - kLowStart] = entry.to;
            } else if (entry.from >= kHighStart && entry.from <= kHighEnd) {
                high[entry.from - kHighStart] = entry.to;
            }
        }
    }
};

} // namespace

char32_t stripAccent(char32_t cp) noexcept {
    if (cp < AccentLookup::kLowStart) {
        return cp;
    }

    static const AccentLookup lookup;
    if (cp <= AccentLookup::kLowEnd) {
        return lookup.low[cp - AccentLookup::kLowStart];
    }
    if (cp >= AccentLookup::kHighStart && cp <= AccentLookup::kHighEnd) {
        return lookup.high[cp - AccentLookup::kHighStart];
    }
    return cp;
}

void normalizeInto(std::string_view text, bool foldCaseToo, bool removeAccents, std::u32string& out) {
    out.clear();
    out.reserve(text.size());

    for (std::size_t i = 0; i < text.size();) {
        char32_t cp = 0;
        i += decodeOne(text, i, cp);

        if (removeAccents) {
            if (isCombiningMark(cp)) {
                continue;
            }
            cp = stripAccent(cp);
        }
        if (foldCaseToo) {
            cp = foldCase(cp);
        }
        out.push_back(cp);
    }
}

void normalizeWithMap(std::string_view text, bool foldCaseToo, bool removeAccents, MappedText& out) {
    out.text.clear();
    out.begin.clear();
    out.end.clear();

    for (std::size_t i = 0; i < text.size();) {
        char32_t cp = 0;
        const std::size_t length = decodeOne(text, i, cp);
        const auto from = static_cast<std::uint32_t>(i);
        i += length;
        const auto to = static_cast<std::uint32_t>(i);

        if (removeAccents) {
            if (isCombiningMark(cp)) {
                if (!out.end.empty()) {
                    out.end.back() = to;
                }
                continue;
            }
            cp = stripAccent(cp);
        }
        if (foldCaseToo) {
            cp = foldCase(cp);
        }
        out.text.push_back(cp);
        out.begin.push_back(from);
        out.end.push_back(to);
    }
}

std::u32string normalize(std::string_view text, bool foldCaseToo, bool removeAccents) {
    std::u32string out;
    normalizeInto(text, foldCaseToo, removeAccents, out);
    return out;
}

} // namespace pfind::utf8
