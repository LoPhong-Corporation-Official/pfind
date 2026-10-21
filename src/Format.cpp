#include "Format.hpp"

#include <charconv>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>

namespace pfind {
namespace {

std::tm toLocalTm(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

std::tm toUtcTm(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm;
}

// Splits "123abc" into the leading number and the remaining suffix.
bool splitNumber(std::string_view text, std::uintmax_t& number, std::string_view& suffix) {
    if (text.empty() || !std::isdigit(static_cast<unsigned char>(text.front()))) {
        return false;
    }

    std::size_t digits = 0;
    while (digits < text.size() && std::isdigit(static_cast<unsigned char>(text[digits]))) {
        ++digits;
    }

    const auto result = std::from_chars(text.data(), text.data() + digits, number);
    if (result.ec != std::errc{}) {
        return false;
    }

    suffix = text.substr(digits);
    return true;
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace

std::string formatSize(std::uintmax_t bytes) {
    constexpr std::uintmax_t KB = 1024;
    constexpr std::uintmax_t MB = KB * 1024;
    constexpr std::uintmax_t GB = MB * 1024;

    std::ostringstream out;
    out << std::fixed << std::setprecision(2);

    if (bytes >= GB) {
        out << static_cast<double>(bytes) / static_cast<double>(GB) << " GB";
    } else if (bytes >= MB) {
        out << static_cast<double>(bytes) / static_cast<double>(MB) << " MB";
    } else if (bytes >= KB) {
        out << static_cast<double>(bytes) / static_cast<double>(KB) << " KB";
    } else {
        out << bytes << " B";
    }

    return out.str();
}

std::optional<std::uintmax_t> parseSize(std::string_view text) {
    std::uintmax_t number = 0;
    std::string_view suffix;
    if (!splitNumber(text, number, suffix)) {
        return std::nullopt;
    }

    std::string s = lower(suffix);
    if (!s.empty() && s.back() == 'b') {
        s.pop_back();
    }

    std::uintmax_t multiplier = 1;
    if (s.empty()) {
        multiplier = 1;
    } else if (s == "k") {
        multiplier = 1ULL << 10;
    } else if (s == "m") {
        multiplier = 1ULL << 20;
    } else if (s == "g") {
        multiplier = 1ULL << 30;
    } else if (s == "t") {
        multiplier = 1ULL << 40;
    } else {
        return std::nullopt;
    }

    if (number > std::numeric_limits<std::uintmax_t>::max() / multiplier) {
        return std::nullopt;
    }
    return number * multiplier;
}

std::optional<std::chrono::seconds> parseDuration(std::string_view text) {
    std::uintmax_t number = 0;
    std::string_view suffix;
    if (!splitNumber(text, number, suffix)) {
        return std::nullopt;
    }

    std::uintmax_t multiplier = 1;
    const std::string s = lower(suffix);
    if (s.empty() || s == "s") {
        multiplier = 1;
    } else if (s == "m") {
        multiplier = 60;
    } else if (s == "h") {
        multiplier = 3600;
    } else if (s == "d") {
        multiplier = 86400;
    } else if (s == "w") {
        multiplier = 604800;
    } else {
        return std::nullopt;
    }

    constexpr auto limit = static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max() / 2);
    if (number > limit / multiplier) {
        return std::nullopt;
    }
    return std::chrono::seconds(static_cast<std::int64_t>(number * multiplier));
}

std::optional<std::size_t> parseUnsigned(std::string_view text) {
    std::uintmax_t number = 0;
    std::string_view suffix;
    if (!splitNumber(text, number, suffix) || !suffix.empty() ||
        number > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(number);
}

std::chrono::system_clock::time_point toSystemTime(std::filesystem::file_time_type time) {
    using namespace std::chrono;
    // Portable replacement for clock_cast (not available in every standard library yet).
    return time_point_cast<system_clock::duration>(
        time - std::filesystem::file_time_type::clock::now() + system_clock::now());
}

std::string formatLocalTime(std::filesystem::file_time_type time) {
    const std::time_t t = std::chrono::system_clock::to_time_t(toSystemTime(time));
    const std::tm tm = toLocalTm(t);
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%d %H:%M", &tm);
    return buffer;
}

std::string formatIsoUtc(std::filesystem::file_time_type time) {
    const std::time_t t = std::chrono::system_clock::to_time_t(toSystemTime(time));
    const std::tm tm = toUtcTm(t);
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

std::string jsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);

    for (const char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }

    return out;
}

} // namespace pfind
