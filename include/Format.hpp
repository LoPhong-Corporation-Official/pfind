#ifndef PFIND_FORMAT_HPP
#define PFIND_FORMAT_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace pfind {

// "1.50 KB", "12 B", ...
std::string formatSize(std::uintmax_t bytes);

// Accepts 123, 10k, 5M, 2G, 1T (binary multiples, case-insensitive, optional trailing 'b').
std::optional<std::uintmax_t> parseSize(std::string_view text);

// Accepts 30s, 15m, 12h, 7d, 2w. A bare number means seconds.
std::optional<std::chrono::seconds> parseDuration(std::string_view text);

// Plain unsigned integer without sign or whitespace.
std::optional<std::size_t> parseUnsigned(std::string_view text);

std::chrono::system_clock::time_point toSystemTime(std::filesystem::file_time_type time);
std::string formatLocalTime(std::filesystem::file_time_type time);  // 2026-10-21 14:03
std::string formatIsoUtc(std::filesystem::file_time_type time);     // 2026-10-21T07:03:00Z

std::string jsonEscape(std::string_view text);

} // namespace pfind

#endif
