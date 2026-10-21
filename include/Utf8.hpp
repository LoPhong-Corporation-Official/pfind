#ifndef PFIND_UTF8_HPP
#define PFIND_UTF8_HPP

#include <filesystem>
#include <string>
#include <string_view>

namespace pfind::utf8 {

// Decodes UTF-8. Invalid bytes are mapped to U+DC00 + byte so nothing is lost.
std::u32string decode(std::string_view text);

// Encodes code points back to UTF-8 (inverse of decode, including escaped bytes).
std::string encode(std::u32string_view text);

// Lossless conversions between UTF-8 strings and filesystem paths on every OS.
std::filesystem::path toPath(std::string_view utf8);
std::string fromPath(const std::filesystem::path& path);
// Same, but always with '/' as the separator.
std::string fromPathGeneric(const std::filesystem::path& path);

// Simple Unicode case folding (ASCII, Latin-1/Extended, Vietnamese, Greek, Cyrillic).
char32_t foldCase(char32_t cp) noexcept;

// True for combining diacritical marks (U+0300..U+036F), used by NFD text (e.g. macOS).
bool isCombiningMark(char32_t cp) noexcept;

// Maps an accented Latin letter to its ASCII base letter; other code points are unchanged.
char32_t stripAccent(char32_t cp) noexcept;

// Same as normalize(), but reuses the caller's buffer (no allocation in hot loops).
void normalizeInto(std::string_view text, bool foldCaseToo, bool removeAccents, std::u32string& out);

// Decodes and optionally folds case and/or removes accents.
std::u32string normalize(std::string_view text, bool foldCaseToo, bool removeAccents);

} // namespace pfind::utf8

#endif
