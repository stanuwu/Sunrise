#include "entity_name_cache.h"
#include "localized_aliases.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../content/items/packages/internal.h"
#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../middleware/content/packages/tables/definition_index_table.h"

namespace sunrise::client::content::entity_names {
namespace {

namespace package_reader = middleware::content::packages::reader;
namespace package_tables = middleware::content::packages::tables;

constexpr std::uint32_t kNamedBagClass = 0x80809478U;
constexpr std::uint32_t kBudgetHeaderClass = 0x808099D1U;
constexpr std::uint32_t kBudgetBodyClass = 0x80809F10U;
constexpr std::uint32_t kEntityClass = 0x80809C0FU;
constexpr std::size_t kNamedBagArrayOffset = 0x08;
constexpr std::size_t kBudgetBodyArrayOffset = 0x20;
constexpr std::size_t kRowSize = 0x10;
constexpr std::size_t kRowTagOffset = 0x08;
constexpr std::size_t kMaximumRows = 1U << 20U;
constexpr std::wstring_view kCacheSuffix = L"\\EntityNames.json";
constexpr std::wstring_view kTemporarySuffix = L".tmp";
constexpr std::string_view kMarker =
    "\"generator\": \"sunrise_package_entity_names_v2\"";

using Name = localized_aliases::Entry;

struct Collection {
    std::vector<Name> names{};
    std::vector<std::uint32_t> related{};
    std::vector<std::uint32_t> validEntities{};
    localized_aliases::Result aliases{};
    std::size_t packages{};
    std::size_t bags{};
};

std::atomic_bool g_attempted{};

template <typename Value>
[[nodiscard]] bool read_value(std::span<const std::byte> blob,
                              std::size_t offset,
                              Value& output) noexcept {
    if (offset > blob.size() || sizeof(Value) > blob.size() - offset) {
        return false;
    }
    std::memcpy(&output, blob.data() + offset, sizeof output);
    return true;
}

[[nodiscard]] bool read_name(std::span<const std::byte> blob,
                             std::size_t pointerOffset,
                             Name& output) noexcept {
    std::int64_t relative = 0;
    if (!read_value(blob, pointerOffset, relative) || relative == 0) {
        return false;
    }
    const std::int64_t startSigned = static_cast<std::int64_t>(pointerOffset) + relative;
    if (startSigned < 0 || static_cast<std::uint64_t>(startSigned) >= blob.size()) {
        return false;
    }

    std::array<char, localized_aliases::kNameCapacity> path{};
    std::size_t length = 0;
    bool terminated = false;
    for (std::size_t cursor = static_cast<std::size_t>(startSigned);
         cursor < blob.size() && length + 1 < path.size();
         ++cursor) {
        const char value = static_cast<char>(blob[cursor]);
        if (value == '\0') {
            terminated = true;
            break;
        }
        path[length++] = value;
    }
    if (!terminated || length == 0) {
        return false;
    }

    std::size_t begin = 0;
    for (std::size_t index = 0; index < length; ++index) {
        if (path[index] == '\\' || path[index] == '/') {
            begin = index + 1;
        }
    }
    std::size_t end = length;
    for (std::size_t index = begin; index < length; ++index) {
        if (path[index] == '.') {
            end = index;
            break;
        }
    }
    if (begin >= end || end - begin >= output.text.size()) {
        return false;
    }
    output.length = static_cast<std::uint8_t>(end - begin);
    std::memcpy(output.text.data(), path.data() + begin, output.length);
    return true;
}

void append_rows(std::span<const std::byte> blob,
                 std::size_t descriptorOffset,
                 Collection& output) noexcept {
    std::int32_t count = 0;
    std::int64_t relative = 0;
    if (!read_value(blob, descriptorOffset, count)
        || !read_value(blob, descriptorOffset + 0x08, relative) || count <= 0
        || static_cast<std::size_t>(count) > kMaximumRows) {
        return;
    }
    const std::int64_t startSigned = static_cast<std::int64_t>(descriptorOffset + 0x18)
                                   + relative;
    if (startSigned < 0) {
        return;
    }
    const std::size_t start = static_cast<std::size_t>(startSigned);
    const std::size_t rows = static_cast<std::size_t>(count);
    if (start > blob.size() || rows > (blob.size() - start) / kRowSize) {
        return;
    }

    for (std::size_t index = 0; index < rows; ++index) {
        const std::size_t row = start + index * kRowSize;
        std::uint32_t tag = 0;
        if (!read_value(blob, row + kRowTagOffset, tag)
            || package_tables::package_of(tag) == package_tables::kAbsentPackageId) {
            continue;
        }
        output.related.push_back(tag);
        Name name{};
        name.tag = tag;
        if (read_name(blob, row, name)) {
            output.names.push_back(name);
        }
    }
}

void sort_unique(std::vector<std::uint32_t>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

[[nodiscard]] bool collect_tag(void* context, std::uint32_t tag) noexcept {
    static_cast<std::vector<std::uint32_t>*>(context)->push_back(tag);
    return true;
}

[[nodiscard]] bool cache_path(core::path::Buffer& output) noexcept {
    HMODULE module = nullptr;
    constexpr DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    return GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&cache_path), &module) != FALSE
        && core::path::artifact_directory(module, output)
        && core::path::append(output, kCacheSuffix);
}

[[nodiscard]] bool current(const wchar_t* path) noexcept {
    const HANDLE file = CreateFileW(path,
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::array<char, 256> header{};
    DWORD read = 0;
    const bool ok = ReadFile(file, header.data(), static_cast<DWORD>(header.size()), &read, nullptr)
                 != FALSE;
    (void)CloseHandle(file);
    return ok && std::string_view(header.data(), read).find(kMarker) != std::string_view::npos;
}

[[nodiscard]] bool extract(std::wstring_view directory, Collection& output) noexcept {
    std::vector<std::uint32_t> bags{};
    package_reader::ScanResult scan{};
    if (!package_reader::scan_class(directory, kNamedBagClass, &collect_tag, &bags, scan)
        || bags.empty()) {
        output.packages = scan.packages;
        return false;
    }
    output.packages = scan.packages;
    output.bags = bags.size();

    package_reader::BlockKeys keys{};
    if (!client::content::items::packages::collect_keys(keys)) {
        return false;
    }
    auto scratch = std::make_unique<package_reader::Scratch>();
    if (!scratch) {
        return false;
    }
    const package_reader::Source source{directory, &keys};
    std::vector<std::byte> blob{};
    std::uint32_t classId = 0;
    for (const std::uint32_t tag : bags) {
        if (package_reader::read_tag(source, *scratch, tag, blob, classId)
            && classId == kNamedBagClass) {
            append_rows(blob, kNamedBagArrayOffset, output);
        }
    }
    sort_unique(output.related);

    std::vector<std::uint32_t> budgetHeaders{};
    for (const std::uint32_t tag : output.related) {
        if (!package_reader::read_tag(source, *scratch, tag, blob, classId)) {
            continue;
        }
        if (classId == kEntityClass) {
            output.validEntities.push_back(tag);
        } else if (classId == kBudgetHeaderClass) {
            budgetHeaders.push_back(tag);
        }
    }

    std::vector<std::uint32_t> budgetBodies{};
    for (const std::uint32_t tag : budgetHeaders) {
        if (!package_reader::read_tag(source, *scratch, tag, blob, classId)
            || classId != kBudgetHeaderClass) {
            continue;
        }
        std::uint32_t bodyTag = 0;
        if (read_value(blob, 0, bodyTag)
            && package_tables::package_of(bodyTag) != package_tables::kAbsentPackageId) {
            budgetBodies.push_back(bodyTag);
        }
    }
    sort_unique(budgetBodies);
    for (const std::uint32_t tag : budgetBodies) {
        if (package_reader::read_tag(source, *scratch, tag, blob, classId)
            && classId == kBudgetBodyClass) {
            append_rows(blob, kBudgetBodyArrayOffset, output);
        }
    }
    sort_unique(output.related);
    sort_unique(output.validEntities);
    const std::vector<std::uint32_t> directEntities = output.validEntities;
    for (const std::uint32_t tag : output.related) {
        if (std::binary_search(directEntities.begin(), directEntities.end(), tag)) {
            continue;
        }
        if (package_reader::read_tag(source, *scratch, tag, blob, classId)
            && classId == kEntityClass) {
            output.validEntities.push_back(tag);
        }
    }
    sort_unique(output.validEntities);
    package_reader::close_files(*scratch);

    output.names.erase(std::remove_if(output.names.begin(),
                                      output.names.end(),
                                      [&output](const Name& name) {
                                          return !std::binary_search(output.validEntities.begin(),
                                                                     output.validEntities.end(),
                                                                     name.tag);
                                      }),
                       output.names.end());
    if (!localized_aliases::append(directory, output.names, output.aliases)) {
        return false;
    }
    std::sort(output.names.begin(), output.names.end(), [](const Name& left, const Name& right) {
        if (left.tag != right.tag) {
            return left.tag < right.tag;
        }
        return std::string_view(left.text.data(), left.length)
             < std::string_view(right.text.data(), right.length);
    });
    output.names.erase(std::unique(output.names.begin(),
                                   output.names.end(),
                                   [](const Name& left, const Name& right) {
                                       return left.tag == right.tag && left.length == right.length
                                           && std::memcmp(left.text.data(),
                                                          right.text.data(),
                                                          left.length) == 0;
                                   }),
                       output.names.end());
    return !output.names.empty();
}

[[nodiscard]] bool write_all(HANDLE file, std::string_view text) noexcept {
    DWORD written = 0;
    return text.size() <= MAXDWORD
        && WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE
        && written == text.size();
}

[[nodiscard]] bool write_escaped(HANDLE file, std::string_view text) noexcept {
    std::array<char, localized_aliases::kNameCapacity * 2 + 1> escaped{};
    std::size_t used = 0;
    for (const unsigned char value : text) {
        if (value == '"' || value == '\\') {
            escaped[used++] = '\\';
            escaped[used++] = static_cast<char>(value);
        } else if (value >= 0x20) {
            escaped[used++] = static_cast<char>(value);
        }
    }
    return write_all(file, {escaped.data(), used});
}

[[nodiscard]] bool write_cache(const core::path::Buffer& path,
                               const std::vector<Name>& names) noexcept {
    core::path::Buffer temporary = path;
    if (!core::path::append(temporary, kTemporarySuffix)) {
        return false;
    }
    (void)DeleteFileW(temporary.chars.data());
    const HANDLE file = CreateFileW(temporary.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool complete = write_all(file,
                              "{\n  \"generator\": \"sunrise_package_entity_names_v2\",\n"
                              "  \"entities\": {\n");
    bool firstTag = true;
    std::size_t cursor = 0;
    while (complete && cursor < names.size()) {
        const std::uint32_t tag = names[cursor].tag;
        std::array<char, 32> prefix{};
        const int length = std::snprintf(prefix.data(),
                                         prefix.size(),
                                         firstTag ? "    \"%08X\": [" : ",\n    \"%08X\": [",
                                         tag);
        complete = length > 0
                && write_all(file, {prefix.data(), static_cast<std::size_t>(length)});
        firstTag = false;
        bool firstName = true;
        while (complete && cursor < names.size() && names[cursor].tag == tag) {
            const Name& name = names[cursor++];
            complete = write_all(file, firstName ? "\"" : ", \"")
                    && write_escaped(file, {name.text.data(), name.length})
                    && write_all(file, "\"");
            firstName = false;
        }
        complete = complete && write_all(file, "]");
    }
    complete = complete && write_all(file, "\n  }\n}\n");
    complete = CloseHandle(file) != FALSE && complete;
    if (complete) {
        complete = MoveFileExW(temporary.chars.data(),
                               path.chars.data(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                   != FALSE;
    }
    if (!complete) {
        (void)DeleteFileW(temporary.chars.data());
    }
    return complete;
}

void report(const Collection& collection, bool written) noexcept {
    std::array<char, 240> line{};
    const int length = std::snprintf(line.data(),
                                     line.size(),
                                     "ev=entity_names stage=extract packages=%zu bags=%zu "
                                     "wrappers=%zu placements=%zu aliases=%zu names=%zu result=%s",
                                     collection.packages,
                                     collection.bags,
                                     collection.aliases.wrappers,
                                     collection.aliases.placements,
                                     collection.aliases.resolved,
                                     collection.names.size(),
                                     written ? "ok" : "fail");
    if (length > 0) {
        core::log::write(core::log::Channel::client,
                         written ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(length)});
    }
}

} // namespace

bool ensure(std::wstring_view packageDirectory) noexcept {
    core::path::Buffer path{};
    if (!cache_path(path)) {
        return false;
    }
    if (current(path.chars.data())) {
        return true;
    }

    package_reader::BlockKeys keys{};
    if (!client::content::items::packages::collect_keys(keys)) {
        return false;
    }
    bool expected = false;
    if (!g_attempted.compare_exchange_strong(expected, true)) {
        return false;
    }

    Collection collection{};
    const bool extracted = extract(packageDirectory, collection);
    const bool written = extracted && write_cache(path, collection.names);
    report(collection, written);
    return written;
}

} // namespace sunrise::client::content::entity_names
