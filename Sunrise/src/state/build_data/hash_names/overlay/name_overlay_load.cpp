#include "name_overlay_load.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>
#include <string_view>

#include "../../../../core/filesystem/path.h"
#include "../../../../core/logging/log.h"
#include "../definition.h"
#include "name_overlay_catalog.h"

namespace sunrise::state::build_data::hash_names::overlay {
namespace {

constexpr std::size_t kFileCapacity = 512 * 1024;
constexpr std::uint32_t kHashBasis = 0x811C9DC5U;
constexpr std::uint32_t kHashPrime = 0x01000193U;
constexpr char kCommentMarker = '#';

struct Report {
    std::size_t lines{};
    std::size_t accepted{};
    std::size_t rejected{};
    std::size_t collisions{};
};

[[nodiscard]] std::uint32_t hash_of(std::string_view text) noexcept {
    std::uint32_t value = kHashBasis;
    for (const char character : text) {
        value = (value * kHashPrime) ^ static_cast<std::uint8_t>(character);
    }
    return value;
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    const auto blank = [](char value) noexcept {
        return value == ' ' || value == '\t' || value == '\r';
    };
    while (!text.empty() && blank(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && blank(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] bool parse(std::string_view line, Name& row) noexcept {
    row = {};
    if (line.empty() || line.size() > kNameLength) {
        return false;
    }
    for (std::size_t index = 0; index < line.size(); ++index) {
        char value = line[index];
        if (value >= 'A' && value <= 'Z') {
            value = static_cast<char>(value - 'A' + 'a');
        }
        if (!name_character(value)) {
            return false;
        }
        row.name[index] = value;
    }
    row.nameLength = static_cast<std::uint8_t>(line.size());
    row.hash = hash_of({row.name.data(), row.nameLength});
    return true;
}

[[nodiscard]] bool
read_file(const wchar_t* path, std::span<char> buffer, std::size_t& size) noexcept {
    size = 0;
    const HANDLE file = CreateFileW(path,
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER measured{};
    DWORD read = 0;
    const bool sized = GetFileSizeEx(file, &measured) != FALSE && measured.QuadPart >= 0
                       && static_cast<std::uint64_t>(measured.QuadPart) <= buffer.size();
    const auto wanted = sized ? static_cast<DWORD>(measured.QuadPart) : 0;
    const bool complete =
        sized
        && (wanted == 0
            || (ReadFile(file, buffer.data(), wanted, &read, nullptr) != FALSE && read == wanted));
    const bool closed = CloseHandle(file) != FALSE;
    size = complete ? static_cast<std::size_t>(read) : 0;
    return complete && closed;
}

[[nodiscard]] std::span<Name>
build_rows(std::string_view document, std::span<Name> rows, Report& report) noexcept {
    report = {};
    std::size_t written = 0;
    std::size_t start = 0;
    for (std::size_t index = 0; index <= document.size(); ++index) {
        if (index != document.size() && document[index] != '\n') {
            continue;
        }
        const std::string_view line = trim(document.substr(start, index - start));
        start = index + 1;
        if (line.empty() || line.front() == kCommentMarker) {
            continue;
        }
        ++report.lines;
        Name row{};
        if (!parse(line, row) || written >= rows.size()) {
            ++report.rejected;
            continue;
        }
        rows[written++] = row;
    }
    const std::span<Name> read = rows.first(written);
    std::stable_sort(read.begin(), read.end(), [](const Name& left, const Name& right) noexcept {
        return left.hash < right.hash;
    });
    std::size_t unique = 0;
    for (std::size_t index = 0; index < read.size(); ++index) {
        if (unique != 0 && read[unique - 1].hash == read[index].hash) {
            if (read[unique - 1].nameLength != read[index].nameLength
                || !std::equal(read[index].name.begin(),
                               read[index].name.begin() + read[index].nameLength,
                               read[unique - 1].name.begin())) {
                ++report.collisions;
            }
            continue;
        }
        read[unique++] = read[index];
    }
    report.accepted = unique;
    return read.first(unique);
}

void report_load(const Report& report, const char* result) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=build_data stage=name_overlay lines=%zu names=%zu "
                                      "rejected=%zu collisions=%zu result=%s",
                                      report.lines,
                                      report.accepted,
                                      report.rejected,
                                      report.collisions,
                                      result);
    if (written <= 0) {
        return;
    }
    const bool lost = report.lines != 0 && report.accepted == 0;
    core::log::write(core::log::Channel::state,
                     lost ? core::log::Level::warn : core::log::Level::info,
                     {line.data(), static_cast<std::size_t>(written)});
}

} // namespace

bool load(void* module) noexcept {
    clear();
    if (module == nullptr) {
        return true;
    }
    core::path::Buffer file;
    if (!core::path::artifact_directory(module, file) || !core::path::append(file, kFileSuffix)) {
        report_load({}, "path");
        return false;
    }
    static std::array<char, kFileCapacity> document{};
    static std::array<Name, kNameCapacity> rows{};
    std::size_t size = 0;
    if (!read_file(file.chars.data(), document, size)) {
        report_load({}, GetLastError() == ERROR_FILE_NOT_FOUND ? "absent" : "read");
        return true;
    }
    Report report{};
    const std::span<const Name> parsed = build_rows({document.data(), size}, rows, report);
    if (!replace(parsed)) {
        report_load(report, "publish");
        return false;
    }
    report_load(report, "ok");
    return true;
}

} // namespace sunrise::state::build_data::hash_names::overlay
