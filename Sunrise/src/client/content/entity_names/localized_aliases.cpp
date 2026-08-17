#include "localized_aliases.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../items/packages/internal.h"
#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../middleware/content/packages/tables/definition_index_table.h"

namespace sunrise::client::content::entity_names::localized_aliases {
namespace {

namespace package_reader = middleware::content::packages::reader;
namespace package_tables = middleware::content::packages::tables;

constexpr std::uint32_t kActivityWrapperClass = 0x80809B14U;
constexpr std::uint32_t kEntityComponentClass = 0x80809C36U;
constexpr std::uint32_t kActivityComponentKind = 0x80809A3BU;
constexpr std::uint32_t kActivityComponentBody = 0x8080948FU;
constexpr std::uint32_t kActivityGroupClass = 0x80808356U;
constexpr std::uint32_t kActivityTableRowClass = 0x80808358U;
constexpr std::uint32_t kEntityNameResourceClass = 0x80807EB6U;
constexpr std::uint32_t kEntityClass = 0x80809C0FU;

constexpr std::uint32_t kStringRootClass = 0x808047B7U;
constexpr std::uint32_t kStringRootRowClass = 0x808047C6U;
constexpr std::uint32_t kLocalizedStringsClass = 0x80809A88U;
constexpr std::uint32_t kLocalizedStringDataClass = 0x80809A8AU;
constexpr std::uint32_t kStringHashClass = 0x80800070U;
constexpr std::uint32_t kStringPartClass = 0x80809A90U;
constexpr std::uint32_t kStringCombinationClass = 0x80809A8EU;

constexpr std::size_t kWrapperComponentTag = 0x0C;
constexpr std::size_t kComponentKindPointer = 0x10;
constexpr std::size_t kComponentBodyPointer = 0x18;
constexpr std::size_t kActivityGroupsDescriptor = 0xA8;
constexpr std::size_t kActivityGroupStride = 0x68;
constexpr std::size_t kActivityTableFirstDescriptor = 0x08;
constexpr std::size_t kActivityTableCount = 6;
constexpr std::size_t kActivityTableRowStride = 0x18;
constexpr std::size_t kMapEntrySize = 0x90;
constexpr std::size_t kMapEntityTag = 0x00;
constexpr std::size_t kMapDataResourcePointer = 0x78;
constexpr std::size_t kEntityNameHash = 0x20;

constexpr std::size_t kStringRootDescriptor = 0x20;
constexpr std::size_t kStringRootRowStride = 0x08;
constexpr std::size_t kStringRootContainerTag = 0x00;
constexpr std::size_t kStringHashesDescriptor = 0x08;
constexpr std::size_t kEnglishStringDataTag = 0x18;
constexpr std::size_t kStringPartsDescriptor = 0x08;
constexpr std::size_t kStringCombinationsDescriptor = 0x48;
constexpr std::size_t kStringHashStride = 0x04;
constexpr std::size_t kStringPartStride = 0x20;
constexpr std::size_t kStringCombinationStride = 0x10;
constexpr std::size_t kPartCharactersPointer = 0x08;
constexpr std::size_t kPartByteLength = 0x14;
constexpr std::size_t kPartCipherShift = 0x18;
constexpr std::size_t kMaximumStringParts = 64;
constexpr std::size_t kMaximumStringBytes = 4096;

struct Candidate {
    std::uint32_t entity{};
    std::uint32_t stringHash{};
};

struct StringValue {
    std::array<char, kNameCapacity> text{};
    std::uint8_t length{};
};

template <typename Value>
[[nodiscard]] bool read(std::span<const std::byte> blob,
                        std::size_t offset,
                        Value& value) noexcept {
    if (offset > blob.size() || sizeof(Value) > blob.size() - offset) {
        return false;
    }
    std::memcpy(&value, blob.data() + offset, sizeof value);
    return true;
}

[[nodiscard]] bool relative(std::span<const std::byte> blob,
                            std::size_t field,
                            std::size_t& output) noexcept {
    std::int64_t delta = 0;
    if (!read(blob, field, delta) || delta == 0) {
        return false;
    }
    const std::int64_t absolute = static_cast<std::int64_t>(field) + delta;
    if (absolute < 0 || static_cast<std::uint64_t>(absolute) >= blob.size()) {
        return false;
    }
    output = static_cast<std::size_t>(absolute);
    return true;
}

[[nodiscard]] bool resource(std::span<const std::byte> blob,
                            std::size_t field,
                            std::size_t& output,
                            std::uint32_t& classId) noexcept {
    if (!relative(blob, field, output) || output < sizeof(classId)
        || !read(blob, output - sizeof(classId), classId)) {
        return false;
    }
    return true;
}

[[nodiscard]] bool collect_tag(void* context, std::uint32_t tag) noexcept {
    static_cast<std::vector<std::uint32_t>*>(context)->push_back(tag);
    return true;
}

void sort_unique(std::vector<std::uint32_t>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

[[nodiscard]] bool scan_tags(std::wstring_view directory,
                             std::uint32_t classId,
                             std::vector<std::uint32_t>& output) noexcept {
    output.clear();
    package_reader::ScanResult scan{};
    const bool ok = package_reader::scan_class(directory, classId, &collect_tag, &output, scan);
    if (ok) {
        sort_unique(output);
    }
    return ok;
}

void append_table_candidates(std::span<const std::byte> blob,
                             const package_tables::Array& table,
                             const std::vector<std::uint32_t>& entityTags,
                             std::vector<Candidate>& output) noexcept {
    if (table.elementClass != kActivityTableRowClass || table.dataOffset > blob.size()
        || table.count > (blob.size() - table.dataOffset) / kActivityTableRowStride) {
        return;
    }
    for (std::uint64_t index = 0; index < table.count; ++index) {
        const std::size_t row = table.dataOffset
                              + static_cast<std::size_t>(index) * kActivityTableRowStride;
        std::size_t mapEntry = 0;
        if (!relative(blob, row, mapEntry) || mapEntry > blob.size()
            || kMapEntrySize > blob.size() - mapEntry) {
            continue;
        }
        std::uint32_t entity = 0;
        if (!read(blob, mapEntry + kMapEntityTag, entity)
            || !std::binary_search(entityTags.begin(), entityTags.end(), entity)) {
            continue;
        }
        std::size_t nameResource = 0;
        std::uint32_t resourceClass = 0;
        std::uint32_t stringHash = 0;
        if (!resource(blob, mapEntry + kMapDataResourcePointer, nameResource, resourceClass)
            || resourceClass != kEntityNameResourceClass
            || !read(blob, nameResource + kEntityNameHash, stringHash) || stringHash == 0
            || stringHash == 0xFFFFFFFFU) {
            continue;
        }
        output.push_back(Candidate{entity, stringHash});
    }
}

void append_wrapper_candidates(const package_reader::Source& source,
                               package_reader::Scratch& scratch,
                               std::uint32_t wrapperTag,
                               const std::vector<std::uint32_t>& entityTags,
                               std::vector<Candidate>& output,
                               std::vector<std::byte>& blob) noexcept {
    std::uint32_t classId = 0;
    if (!package_reader::read_tag(source, scratch, wrapperTag, blob, classId)
        || classId != kActivityWrapperClass) {
        return;
    }
    std::uint32_t componentTag = 0;
    if (!read(blob, kWrapperComponentTag, componentTag)) {
        return;
    }
    if (!package_reader::read_tag(source, scratch, componentTag, blob, classId)
        || classId != kEntityComponentClass) {
        return;
    }

    std::size_t kind = 0;
    std::size_t body = 0;
    std::uint32_t kindClass = 0;
    std::uint32_t bodyClass = 0;
    if (!resource(blob, kComponentKindPointer, kind, kindClass)
        || kindClass != kActivityComponentKind
        || !resource(blob, kComponentBodyPointer, body, bodyClass)
        || bodyClass != kActivityComponentBody) {
        return;
    }

    package_tables::Array groups{};
    if (!package_tables::find_array_at(blob, body + kActivityGroupsDescriptor, groups)
        || groups.elementClass != kActivityGroupClass || groups.dataOffset > blob.size()
        || groups.count > (blob.size() - groups.dataOffset) / kActivityGroupStride) {
        return;
    }
    for (std::uint64_t groupIndex = 0; groupIndex < groups.count; ++groupIndex) {
        const std::size_t group = groups.dataOffset
                                + static_cast<std::size_t>(groupIndex) * kActivityGroupStride;
        for (std::size_t tableIndex = 0; tableIndex < kActivityTableCount; ++tableIndex) {
            package_tables::Array table{};
            const std::size_t descriptor = group + kActivityTableFirstDescriptor
                                         + tableIndex * 0x10;
            if (package_tables::find_array_at(blob, descriptor, table)) {
                append_table_candidates(blob, table, entityTags, output);
            }
        }
    }
}

[[nodiscard]] bool collect_candidates(const package_reader::Source& source,
                                      package_reader::Scratch& scratch,
                                      std::vector<Candidate>& output,
                                      Result& result) noexcept {
    std::vector<std::uint32_t> wrappers{};
    std::vector<std::uint32_t> entities{};
    if (!scan_tags(source.directory, kActivityWrapperClass, wrappers)
        || !scan_tags(source.directory, kEntityClass, entities) || wrappers.empty()
        || entities.empty()) {
        return false;
    }
    result.wrappers = wrappers.size();
    std::vector<std::byte> blob{};
    for (const std::uint32_t wrapper : wrappers) {
        append_wrapper_candidates(source, scratch, wrapper, entities, output, blob);
    }
    std::sort(output.begin(), output.end(), [](const Candidate& left, const Candidate& right) {
        return left.entity < right.entity
            || (left.entity == right.entity && left.stringHash < right.stringHash);
    });
    output.erase(std::unique(output.begin(), output.end(), [](const Candidate& left,
                                                              const Candidate& right) {
                     return left.entity == right.entity && left.stringHash == right.stringHash;
                 }),
                 output.end());
    result.placements = output.size();
    return !output.empty();
}

[[nodiscard]] bool collect_string_containers(const package_reader::Source& source,
                                             package_reader::Scratch& scratch,
                                             std::vector<std::uint32_t>& output) noexcept {
    std::vector<std::uint32_t> roots{};
    if (!scan_tags(source.directory, kStringRootClass, roots)) {
        return false;
    }
    std::vector<std::byte> blob{};
    std::uint32_t classId = 0;
    for (const std::uint32_t root : roots) {
        package_tables::Array rows{};
        if (!package_reader::read_tag(source, scratch, root, blob, classId)
            || classId != kStringRootClass
            || !package_tables::find_array_at(blob, kStringRootDescriptor, rows)
            || rows.elementClass != kStringRootRowClass || rows.dataOffset > blob.size()
            || rows.count > (blob.size() - rows.dataOffset) / kStringRootRowStride) {
            continue;
        }
        for (std::uint64_t index = 0; index < rows.count; ++index) {
            std::uint32_t tag = 0;
            const std::size_t row = rows.dataOffset
                                  + static_cast<std::size_t>(index) * kStringRootRowStride;
            if (read(blob, row + kStringRootContainerTag, tag)) {
                output.push_back(tag);
            }
        }
    }
    sort_unique(output);
    if (!output.empty()) {
        return true;
    }
    return scan_tags(source.directory, kLocalizedStringsClass, output) && !output.empty();
}

[[nodiscard]] bool find_string_index(std::span<const std::byte> localized,
                                     const package_tables::Array& hashes,
                                     std::uint32_t wanted,
                                     std::size_t& output) noexcept {
    if (hashes.elementClass != kStringHashClass || hashes.dataOffset > localized.size()
        || hashes.count > (localized.size() - hashes.dataOffset) / kStringHashStride) {
        return false;
    }
    std::uint64_t low = 0;
    std::uint64_t high = hashes.count;
    while (low < high) {
        const std::uint64_t middle = low + (high - low) / 2;
        std::uint32_t value = 0;
        if (!read(localized,
                  hashes.dataOffset + static_cast<std::size_t>(middle) * kStringHashStride,
                  value)) {
            return false;
        }
        if (value < wanted) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    std::uint32_t value = 0;
    if (low >= hashes.count
        || !read(localized,
                 hashes.dataOffset + static_cast<std::size_t>(low) * kStringHashStride,
                 value)
        || value != wanted) {
        return false;
    }
    output = static_cast<std::size_t>(low);
    return true;
}

[[nodiscard]] bool decode_string(std::span<const std::byte> data,
                                 const package_tables::Array& parts,
                                 const package_tables::Array& combinations,
                                 std::size_t stringIndex,
                                 std::string& output) {
    output.clear();
    if (parts.elementClass != kStringPartClass
        || combinations.elementClass != kStringCombinationClass
        || stringIndex >= combinations.count || parts.dataOffset > data.size()
        || parts.count > (data.size() - parts.dataOffset) / kStringPartStride
        || combinations.dataOffset > data.size()
        || combinations.count
               > (data.size() - combinations.dataOffset) / kStringCombinationStride) {
        return false;
    }
    const std::size_t combination = combinations.dataOffset
                                  + stringIndex * kStringCombinationStride;
    std::int64_t partCount = 0;
    std::size_t firstPart = 0;
    if (!read(data, combination + 8, partCount) || partCount <= 0
        || partCount > static_cast<std::int64_t>(kMaximumStringParts)
        || !relative(data, combination, firstPart) || firstPart < parts.dataOffset
        || (firstPart - parts.dataOffset) % kStringPartStride != 0) {
        return false;
    }
    const std::size_t firstIndex = (firstPart - parts.dataOffset) / kStringPartStride;
    if (firstIndex >= parts.count
        || static_cast<std::uint64_t>(partCount) > parts.count - firstIndex) {
        return false;
    }
    for (std::size_t index = 0; index < static_cast<std::size_t>(partCount); ++index) {
        const std::size_t part = parts.dataOffset + (firstIndex + index) * kStringPartStride;
        std::size_t characters = 0;
        std::uint16_t byteLength = 0;
        std::uint16_t cipherShift = 0;
        if (!relative(data, part + kPartCharactersPointer, characters)
            || !read(data, part + kPartByteLength, byteLength)
            || !read(data, part + kPartCipherShift, cipherShift)
            || byteLength > kMaximumStringBytes - output.size() || characters > data.size()
            || byteLength > data.size() - characters) {
            return false;
        }
        const char* bytes = reinterpret_cast<const char*>(data.data() + characters);
        if (cipherShift == 0) {
            output.append(bytes, byteLength);
        } else {
            for (std::size_t byte = 0; byte < byteLength; ++byte) {
                output.push_back(static_cast<char>(
                    static_cast<unsigned char>(bytes[byte]) + cipherShift));
            }
        }
    }
    output.erase(std::find(output.begin(), output.end(), '\0'), output.end());
    return !output.empty();
}

void store_string(std::string_view source, StringValue& output) noexcept {
    if (source.empty() || source.size() >= output.text.size()) {
        return;
    }
    std::size_t length = 0;
    for (const unsigned char value : source) {
        if (value >= 0x20) {
            output.text[length++] = static_cast<char>(value);
        }
    }
    if (length != 0) {
        output.length = static_cast<std::uint8_t>(length);
    }
}

void resolve_container(const package_reader::Source& source,
                       package_reader::Scratch& scratch,
                       std::uint32_t container,
                       const std::vector<std::uint32_t>& wanted,
                       std::vector<StringValue>& values,
                       std::vector<std::byte>& localized,
                       std::vector<std::byte>& data) {
    std::uint32_t classId = 0;
    package_tables::Array hashes{};
    if (!package_reader::read_tag(source, scratch, container, localized, classId)
        || classId != kLocalizedStringsClass
        || !package_tables::find_array_at(localized, kStringHashesDescriptor, hashes)) {
        return;
    }

    std::vector<std::pair<std::size_t, std::size_t>> matches{};
    for (std::size_t wantedIndex = 0; wantedIndex < wanted.size(); ++wantedIndex) {
        if (values[wantedIndex].length != 0) {
            continue;
        }
        std::size_t stringIndex = 0;
        if (find_string_index(localized, hashes, wanted[wantedIndex], stringIndex)) {
            matches.emplace_back(wantedIndex, stringIndex);
        }
    }
    if (matches.empty()) {
        return;
    }

    std::uint32_t dataTag = 0;
    package_tables::Array parts{};
    package_tables::Array combinations{};
    if (!read(localized, kEnglishStringDataTag, dataTag)
        || !package_reader::read_tag(source, scratch, dataTag, data, classId)
        || classId != kLocalizedStringDataClass
        || !package_tables::find_array_at(data, kStringPartsDescriptor, parts)
        || !package_tables::find_array_at(data, kStringCombinationsDescriptor, combinations)) {
        return;
    }
    std::string decoded{};
    for (const auto [wantedIndex, stringIndex] : matches) {
        if (decode_string(data, parts, combinations, stringIndex, decoded)) {
            store_string(decoded, values[wantedIndex]);
        }
    }
}

[[nodiscard]] bool resolve_candidates(const package_reader::Source& source,
                                      package_reader::Scratch& scratch,
                                      const std::vector<Candidate>& candidates,
                                      std::vector<Entry>& output,
                                      Result& result) {
    std::vector<std::uint32_t> containers{};
    if (!collect_string_containers(source, scratch, containers)) {
        return false;
    }
    std::vector<std::uint32_t> wanted{};
    wanted.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
        wanted.push_back(candidate.stringHash);
    }
    sort_unique(wanted);
    std::vector<StringValue> values(wanted.size());
    std::vector<std::byte> localized{};
    std::vector<std::byte> data{};
    for (const std::uint32_t container : containers) {
        resolve_container(source, scratch, container, wanted, values, localized, data);
    }

    const std::size_t before = output.size();
    for (const Candidate& candidate : candidates) {
        const auto found = std::lower_bound(wanted.begin(), wanted.end(), candidate.stringHash);
        if (found == wanted.end() || *found != candidate.stringHash) {
            continue;
        }
        const StringValue& value = values[static_cast<std::size_t>(found - wanted.begin())];
        if (value.length == 0) {
            continue;
        }
        Entry entry{};
        entry.tag = candidate.entity;
        entry.length = value.length;
        std::memcpy(entry.text.data(), value.text.data(), value.length);
        output.push_back(entry);
    }
    result.resolved = output.size() - before;
    return result.resolved != 0;
}

} // namespace

bool append(std::wstring_view packageDirectory,
            std::vector<Entry>& output,
            Result& result) noexcept {
    result = {};
    package_reader::BlockKeys keys{};
    if (!client::content::items::packages::collect_keys(keys)) {
        return false;
    }
    auto scratch = std::make_unique<package_reader::Scratch>();
    if (!scratch) {
        return false;
    }
    const package_reader::Source source{packageDirectory, &keys};
    std::vector<Candidate> candidates{};
    const bool collected = collect_candidates(source, *scratch, candidates, result);
    const bool resolved = collected
                       && resolve_candidates(source, *scratch, candidates, output, result);
    package_reader::close_files(*scratch);
    return resolved;
}

} // namespace sunrise::client::content::entity_names::localized_aliases
