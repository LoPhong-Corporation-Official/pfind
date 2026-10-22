#include "Replace.hpp"

#include "Utf8.hpp"

#include <fstream>
#include <iterator>
#include <system_error>

namespace pfind {
namespace fs = std::filesystem;
namespace {

constexpr std::uintmax_t kMaxFileBytes = 256ULL * 1024 * 1024;

std::string display(std::string_view line) {
    constexpr std::size_t kMaxShownBytes = 300;
    if (line.size() <= kMaxShownBytes) {
        return std::string(line);
    }
    std::size_t cut = kMaxShownBytes;
    while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return std::string(line.substr(0, cut)) + "...";
}

} // namespace

ReplaceResult replaceInFile(const fs::path& path,
                            const Matcher& matcher,
                            std::string_view replacement,
                            bool apply,
                            bool backup) {
    ReplaceResult result;

    try {
        std::error_code ec;
        const fs::path target = fs::canonical(path, ec);   // resolves symbolic links
        if (ec) {
            result.error = "Cannot resolve path (" + ec.message() + ")";
            return result;
        }

        const std::uintmax_t size = fs::file_size(target, ec);
        if (ec) {
            result.error = "Cannot read size (" + ec.message() + ")";
            return result;
        }
        if (size > kMaxFileBytes) {
            result.error = "File too large to replace in";
            return result;
        }

        std::string content;
        {
            std::ifstream in(target, std::ios::binary);
            if (!in) {
                result.error = "Cannot open file";
                return result;
            }
            content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        if (content.substr(0, 8192).find('\0') != std::string::npos) {
            return result;   // binary file: nothing to do
        }

        std::string output;
        output.reserve(content.size());

        std::size_t position = 0;
        std::size_t lineNumber = 0;
        while (position < content.size()) {
            const std::size_t newline = content.find('\n', position);
            const bool hasNewline = newline != std::string::npos;
            const std::size_t end = hasNewline ? newline : content.size();

            std::string_view line(content.data() + position, end - position);
            std::string_view carriageReturn;
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
                carriageReturn = "\r";
            }
            ++lineNumber;

            bool replaced = false;
            if (matcher.matches(line)) {
                std::size_t count = 0;
                std::string changed = matcher.replaceAll(line, replacement, count);
                if (count > 0 && changed != line) {
                    result.replacements += count;
                    result.changes.push_back({lineNumber, display(line), display(changed)});
                    output += changed;
                    replaced = true;
                }
            }
            if (!replaced) {
                output.append(line);
            }

            output.append(carriageReturn);
            if (hasNewline) {
                output += '\n';
            }
            position = hasNewline ? newline + 1 : content.size();
        }

        if (result.changes.empty() || !apply) {
            return result;
        }

        if (backup) {
            const fs::path backupPath = utf8::toPath(utf8::fromPath(target) + ".bak");
            fs::copy_file(target, backupPath, fs::copy_options::overwrite_existing);
        }

        const fs::path temporary =
            target.parent_path() / utf8::toPath(utf8::fromPath(target.filename()) + ".pfind-tmp");

        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out) {
                result.error = "Cannot create temporary file next to the original";
                return result;
            }
            out.write(output.data(), static_cast<std::streamsize>(output.size()));
            out.flush();
            if (!out) {
                out.close();
                fs::remove(temporary, ec);
                result.error = "Cannot write temporary file (disk full?)";
                return result;
            }
        }

        fs::permissions(temporary, fs::status(target).permissions(), ec);   // best effort
        fs::rename(temporary, target, ec);
        if (ec) {
            const std::string message = ec.message();
            fs::remove(temporary, ec);
            result.error = "Cannot replace the file (" + message + ")";
            return result;
        }

        result.written = true;
    } catch (const std::exception& exception) {
        result.error = std::string("Replace failed (") + exception.what() + ")";
    }

    return result;
}

} // namespace pfind
