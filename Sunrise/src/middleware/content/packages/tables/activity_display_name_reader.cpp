#include "activity_display_name_reader.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "../reader/layout.h"
#include "../schema/generated/tag_classes_808000.h"
#include "../schema/generated/tag_classes_80805e.h"
#include "../schema/generated/tag_classes_80805f.h"
#include "../schema/generated/tag_classes_80809a.h"
#include "activity_presentation_table.h"
#include "internal.h"

namespace sunrise::middleware::content::packages::tables::activity_display_names {
namespace {

namespace generated = schema::generated;
/** A descriptor names a typed array header; its rows follow it. */
using Header = reader::layout::TypedArrayHeader;

/** Field offsets, strides and element classes of the installed string-bank layouts. */
constexpr std::size_t kActivityIndexFieldOffset = offsetof(generated::ActivityClientTable, table);
constexpr std::size_t kActivityIndexHashOffset = offsetof(generated::ActivityClientIndexRow, hash);
constexpr std::size_t kActivityIndexPaddingOffset =
    offsetof(generated::ActivityClientIndexRow, gap4);
constexpr std::uint32_t kActivityIndexElementClass = generated::ActivityClientIndexRow::kClass;
constexpr std::size_t kDisplayFieldOffset =
    offsetof(generated::ActivityClientTable, componentStorage.displayRows);
constexpr std::size_t kDisplayStride = sizeof(generated::ActivityDisplayRow);
constexpr std::uint32_t kDisplayElementClass = generated::ActivityDisplayRow::kClass;
constexpr std::size_t kBankIndexFieldOffset = offsetof(generated::StringBankIndex, table);
constexpr std::size_t kBankIndexStride = sizeof(generated::StringBankRow);
constexpr std::size_t kBankNameHashOffset = offsetof(generated::StringBankRow, nameHash);
constexpr std::size_t kBankContainerOffset = offsetof(generated::StringBankRow, container);
constexpr std::uint32_t kBankIndexElementClass = generated::StringBankRow::kClass;
constexpr std::size_t kContainerHashFieldOffset = offsetof(generated::SLocalizedStrings, ids);
constexpr std::uint32_t kContainerHashElementClass = generated::SU32HashElement::kClass;
/** English is the first language slot; a container must hold every slot through the last. */
constexpr std::size_t kContainerEnglishOffset =
    offsetof(generated::SLocalizedStrings, languageData);
constexpr std::size_t kContainerLanguageEnd =
    offsetof(generated::SLocalizedStrings, languageDataRussian)
    + sizeof(generated::SLocalizedStrings::languageDataRussian);
/** Field offsets, strides and element classes of the installed combination layouts. */
constexpr std::uint32_t kCombinationElementClass = generated::SStringCombination::kClass;
constexpr std::size_t kPartHashOffset = offsetof(generated::SStringPart, variableHash);
constexpr std::size_t kPartStringLengthOffset = offsetof(generated::SStringPart, stringLength);
/** Ceilings that stop a corrupt array header from driving an unbounded read. */
constexpr std::size_t kMaximumParts = 64;
constexpr std::size_t kMaximumArrayRows = 1'000'000;
/** This hash is the no-name value, so a part carrying it contributes literal text. */
constexpr std::uint32_t kLiteralPartHash = 0x811C9DC5U;

struct DisplayReference final {
    std::uint16_t bankIndex{};
    std::uint32_t stringHash{};
};

struct Bank final {
    std::uint32_t nameHash{};
    std::uint32_t containerTag{};
};

struct Array final {
    std::size_t count{};
    std::size_t data{};
};

struct HashRow final {
    std::uint32_t hash{};
    std::uint32_t index{};
};

struct LoadedBank final {
    std::vector<HashRow> hashes{};
    std::vector<std::byte> language{};
    Array combinations{};
    bool loaded{};
};

/** One loaded bank and the container it came from. One resolve pass reads a container once. */
struct LoadedBankEntry final {
    std::uint32_t containerTag{};
    LoadedBank bank{};
};

enum class ResolveStatus : std::uint8_t {
    resolved,
    absent,
    invalid,
};

/** Resolves one table pointer, accepting the canonical null descriptor for an empty array. */
[[nodiscard]] bool read_array(std::span<const std::byte> blob,
                              std::size_t field,
                              std::size_t stride,
                              std::size_t maximum,
                              std::uint32_t expectedElementClass,
                              Array& output) noexcept {
    output = {};
    std::uint64_t count = 0;
    std::int64_t relative = 0;
    std::size_t header = 0;
    std::uint64_t repeated = 0;
    std::uint32_t elementClass = 0;
    std::uint32_t headerPadding = 0;
    if (!read(blob, field, count) || count > maximum
        || !read(blob, field + reader::layout::kArrayFieldRelative, relative)) {
        return false;
    }
    if (count == 0 && relative == 0) {
        return true;
    }
    if (!relative_offset(field + reader::layout::kArrayFieldRelative, relative, header)
        || header > blob.size() || Header::kSize > blob.size() - header
        || !read(blob, header + Header::kCount, repeated) || repeated != count
        || !read(blob, header + Header::kClass, elementClass)
        || elementClass != expectedElementClass
        || !read(blob, header + Header::kPadding, headerPadding) || headerPadding != 0
        || count > (std::numeric_limits<std::size_t>::max)() / stride) {
        return false;
    }
    const std::size_t bytes = static_cast<std::size_t>(count) * stride;
    const std::size_t data = header + Header::kSize;
    if (data > blob.size() || bytes > blob.size() - data) {
        return false;
    }
    output = {static_cast<std::size_t>(count), data};
    return true;
}

/** Appends one Unicode scalar as strict UTF-8 into bounded name storage. */
[[nodiscard]] bool append_codepoint(std::uint32_t codepoint, Name& output) noexcept {
    if (codepoint == 0 || codepoint > 0x10FFFFU || (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
        return false;
    }
    std::array<char, 4> encoded{};
    std::size_t count = 0;
    if (codepoint <= 0x7FU) {
        encoded[0] = static_cast<char>(codepoint);
        count = 1;
    } else if (codepoint <= 0x7FFU) {
        encoded[0] = static_cast<char>(0xC0U | (codepoint >> 6U));
        encoded[1] = static_cast<char>(0x80U | (codepoint & 0x3FU));
        count = 2;
    } else if (codepoint <= 0xFFFFU) {
        encoded[0] = static_cast<char>(0xE0U | (codepoint >> 12U));
        encoded[1] = static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
        encoded[2] = static_cast<char>(0x80U | (codepoint & 0x3FU));
        count = 3;
    } else {
        encoded[0] = static_cast<char>(0xF0U | (codepoint >> 18U));
        encoded[1] = static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU));
        encoded[2] = static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
        encoded[3] = static_cast<char>(0x80U | (codepoint & 0x3FU));
        count = 4;
    }
    if (count >= output.value.size() - output.length) {
        return false;
    }
    std::copy_n(encoded.begin(), count, output.value.begin() + output.length);
    output.length = static_cast<std::uint16_t>(output.length + count);
    return true;
}

/** Decodes strict UTF-8, applies the authored scalar cipher, and appends exact UTF-8. */
[[nodiscard]] bool append_text(std::span<const std::byte> bytes,
                               std::uint16_t cipher,
                               Name& output,
                               std::uint16_t& scalarCount) noexcept {
    scalarCount = 0;
    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
        const auto first = std::to_integer<std::uint8_t>(bytes[cursor]);
        std::size_t count = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if (first <= 0x7FU) {
            count = 1;
            codepoint = first;
        } else if (first >= 0xC2U && first <= 0xDFU) {
            count = 2;
            codepoint = first & 0x1FU;
            minimum = 0x80U;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            count = 3;
            codepoint = first & 0x0FU;
            minimum = 0x800U;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            count = 4;
            codepoint = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (count > bytes.size() - cursor) {
            return false;
        }
        for (std::size_t index = 1; index < count; ++index) {
            const auto next = std::to_integer<std::uint8_t>(bytes[cursor + index]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (next & 0x3FU);
        }
        if (codepoint < minimum || codepoint > 0x10FFFFU
            || (codepoint >= 0xD800U && codepoint <= 0xDFFFU) || cipher > 0x10FFFFU - codepoint
            || !append_codepoint(codepoint + cipher, output)
            || scalarCount == (std::numeric_limits<std::uint16_t>::max)()) {
            return false;
        }
        ++scalarCount;
        cursor += count;
    }
    return true;
}

/** Reads the client table's complete activity-to-display-reference join. */
[[nodiscard]] bool read_references(std::span<const std::byte> blob,
                                   std::span<const std::uint32_t> definitionHashes,
                                   std::vector<DisplayReference>& output) {
    output.clear();
    if (definitionHashes.empty()
        || definitionHashes.size() >= (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    Array indexRows{};
    Array displayRows{};
    if (!read_array(blob,
                    kActivityIndexFieldOffset,
                    activities::kDisplayRowStride,
                    definitionHashes.size(),
                    kActivityIndexElementClass,
                    indexRows)
        || indexRows.count != definitionHashes.size()
        || !read_array(blob,
                       kDisplayFieldOffset,
                       kDisplayStride,
                       kMaximumArrayRows,
                       kDisplayElementClass,
                       displayRows)
        || displayRows.count == 0
        || indexRows.count
               > (std::numeric_limits<std::size_t>::max)() / activities::kDisplayRowStride) {
        return false;
    }
    output.resize(indexRows.count);
    const std::size_t indexEnd = indexRows.data + indexRows.count * activities::kDisplayRowStride;
    const std::size_t displayEnd = displayRows.data + displayRows.count * kDisplayStride;
    // A merged table moves its index to its end, past the records a mod added. A mod's record
    // carries its own text block, so a text block need not be a row of the installed array.
    const auto inside_index = [&](std::size_t offset) noexcept {
        return offset >= indexRows.data && offset < indexEnd;
    };
    const auto inside_array = [&](std::size_t offset) noexcept {
        return inside_index(offset) || (offset >= displayRows.data && offset < displayEnd);
    };
    std::size_t previousRecord = 0;
    for (std::size_t index = 0; index < output.size(); ++index) {
        const std::size_t row = indexRows.data + index * activities::kDisplayRowStride;
        std::uint32_t hash = 0;
        std::uint32_t padding = 0;
        std::int64_t recordRelative = 0;
        std::size_t record = 0;
        std::int64_t displayRelative = 0;
        std::size_t display = 0;
        if (!read(blob, row + kActivityIndexHashOffset, hash) || hash != definitionHashes[index]
            || !read(blob, row + kActivityIndexPaddingOffset, padding) || padding != 0
            || !read(blob, row + activities::kDisplayRecordField, recordRelative)
            || !relative_offset(row + activities::kDisplayRecordField, recordRelative, record)
            || inside_array(record) || (index != 0 && record <= previousRecord)
            || !read(blob, record + activities::kDisplayTextField, displayRelative)
            || !relative_offset(record + activities::kDisplayTextField, displayRelative, display)
            || inside_index(display)
            || !read(blob, display + activities::kTextTitleBankOffset, output[index].bankIndex)
            || !read(blob, display + activities::kTextTitleHashOffset, output[index].stringHash)) {
            return false;
        }
        previousRecord = record;
    }
    return true;
}

/** Reads the exact installed bank-index array. */
[[nodiscard]] bool read_banks(std::span<const std::byte> blob, std::vector<Bank>& output) {
    output.clear();
    Array rows{};
    if (!read_array(blob,
                    kBankIndexFieldOffset,
                    kBankIndexStride,
                    kMaximumArrayRows,
                    kBankIndexElementClass,
                    rows)
        || rows.count == 0) {
        return false;
    }
    output.resize(rows.count);
    for (std::size_t index = 0; index < output.size(); ++index) {
        const std::size_t offset = rows.data + index * kBankIndexStride;
        if (!read(blob, offset + kBankNameHashOffset, output[index].nameHash)
            || !read(blob, offset + kBankContainerOffset, output[index].containerTag)
            || output[index].nameHash == 0 || output[index].containerTag == 0) {
            return false;
        }
    }
    return true;
}

/** Reads one selected combination and accepts literal parts only. */
[[nodiscard]] bool read_combination(std::span<const std::byte> language,
                                    const Array& combinations,
                                    std::size_t index,
                                    Name& output) noexcept {
    if (index >= combinations.count
        || index > ((std::numeric_limits<std::size_t>::max)() - combinations.data)
                       / activities::kCombinationStride) {
        return false;
    }
    const std::size_t combination = combinations.data + index * activities::kCombinationStride;
    std::int64_t partsRelative = 0;
    std::int64_t partCount = 0;
    std::size_t parts = 0;
    if (!read(language, combination + activities::kCombinationPartsField, partsRelative)
        || !relative_offset(combination + activities::kCombinationPartsField, partsRelative, parts)
        || !read(language, combination + activities::kCombinationCountOffset, partCount)
        || partCount < 0 || static_cast<std::uint64_t>(partCount) > kMaximumParts
        || static_cast<std::uint64_t>(partCount)
               > (std::numeric_limits<std::size_t>::max)() / activities::kPartStride) {
        return false;
    }
    const std::size_t bytes = static_cast<std::size_t>(partCount) * activities::kPartStride;
    if (parts > language.size() || bytes > language.size() - parts) {
        return false;
    }
    for (std::size_t indexPart = 0; indexPart < static_cast<std::size_t>(partCount); ++indexPart) {
        const std::size_t part = parts + indexPart * activities::kPartStride;
        std::int64_t textRelative = 0;
        std::size_t text = 0;
        std::uint32_t valueHash = 0;
        std::uint16_t byteLength = 0;
        std::uint16_t stringLength = 0;
        std::uint16_t cipher = 0;
        std::uint16_t decodedScalars = 0;
        if (!read(language, part + activities::kPartDataField, textRelative)
            || !relative_offset(part + activities::kPartDataField, textRelative, text)
            || !read(language, part + kPartHashOffset, valueHash) || valueHash != kLiteralPartHash
            || !read(language, part + activities::kPartByteCountOffset, byteLength)
            || !read(language, part + kPartStringLengthOffset, stringLength)
            || !read(language, part + activities::kPartShiftOffset, cipher)
            || text > language.size() || byteLength > language.size() - text
            || (byteLength == 0) != (stringLength == 0)
            || !append_text(language.subspan(text, byteLength), cipher, output, decodedScalars)
            || decodedScalars != stringLength) {
            return false;
        }
    }
    return true;
}

/** Reads one exact bank once, rejecting duplicate hashes and language-count drift. */
[[nodiscard]] bool load_bank(const Source& source, const Bank& bank, LoadedBank& output) {
    output = {};
    std::vector<std::byte> container;
    if (source.read == nullptr
        || !source.read(source.context, bank.containerTag, kStringContainerClass, container)) {
        return false;
    }
    Array hashes{};
    if (!read_array(container,
                    kContainerHashFieldOffset,
                    activities::kBankHashStride,
                    kMaximumArrayRows,
                    kContainerHashElementClass,
                    hashes)
        || kContainerLanguageEnd > container.size()) {
        return false;
    }
    output.hashes.reserve(hashes.count);
    for (std::size_t index = 0; index < hashes.count; ++index) {
        std::uint32_t candidate = 0;
        if (!read(container, hashes.data + index * activities::kBankHashStride, candidate)) {
            return false;
        }
        if (index >= (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
        output.hashes.push_back({candidate, static_cast<std::uint32_t>(index)});
    }
    std::sort(
        output.hashes.begin(), output.hashes.end(), [](const HashRow& row, const HashRow& other) {
            return row.hash != other.hash ? row.hash < other.hash : row.index < other.index;
        });
    if (std::adjacent_find(output.hashes.begin(),
                           output.hashes.end(),
                           [](const HashRow& left, const HashRow& right) {
                               return left.hash == right.hash && left.hash != kLiteralPartHash;
                           })
        != output.hashes.end()) {
        return false;
    }
    std::uint32_t languageTag = 0;
    if (!read(container, kContainerEnglishOffset, languageTag) || languageTag == 0
        || !source.read(source.context, languageTag, kLanguageDataClass, output.language)
        || !read_array(output.language,
                       activities::kLanguageCombinationDescriptor,
                       activities::kCombinationStride,
                       kMaximumArrayRows,
                       kCombinationElementClass,
                       output.combinations)
        || output.combinations.count != hashes.count) {
        return false;
    }
    output.loaded = true;
    return true;
}

/** Resolves one exact hash, distinguishing absent authored text from malformed bank data. */
[[nodiscard]] ResolveStatus
resolve_name(const LoadedBank& bank, std::uint32_t stringHash, Name& output) noexcept {
    if (!bank.loaded) {
        return ResolveStatus::invalid;
    }
    const auto found = std::lower_bound(
        bank.hashes.begin(), bank.hashes.end(), stringHash, [](const HashRow& row, auto value) {
            return row.hash < value;
        });
    if (found == bank.hashes.end() || found->hash != stringHash) {
        return ResolveStatus::absent;
    }
    return read_combination(bank.language, bank.combinations, found->index, output)
                   && output.length != 0
               ? ResolveStatus::resolved
               : ResolveStatus::invalid;
}

} // namespace

/** Reads all activity display references and resolves exact English strings by bank and hash. */
bool build(const Source& source,
           std::span<const std::uint32_t> definitionHashes,
           Snapshot& output) noexcept {
    output = {};
    if (source.read == nullptr || source.activityTableTag == 0 || source.stringBankIndexTag == 0
        || definitionHashes.empty()) {
        return false;
    }
    try {
        std::vector<std::byte> activityBlob;
        std::vector<std::byte> bankBlob;
        std::vector<DisplayReference> references{};
        std::vector<Bank> banks{};
        if (!source.read(
                source.context, source.activityTableTag, kActivityClientTableClass, activityBlob)
            || !read_references(activityBlob, definitionHashes, references)
            || !source.read(
                source.context, source.stringBankIndexTag, kStringBankIndexClass, bankBlob)
            || !read_banks(bankBlob, banks)) {
            return false;
        }

        Snapshot pending{};
        pending.names.resize(references.size());
        std::vector<LoadedBank> loadedBanks(banks.size());
        for (std::size_t index = 0; index < pending.names.size(); ++index) {
            Name& name = pending.names[index];
            name.bankIndex = references[index].bankIndex;
            name.stringHash = references[index].stringHash;
            if (name.stringHash == kLiteralPartHash) {
                name.authoredEmpty = true;
                ++pending.authoredEmptyCount;
                continue;
            }
            if (name.bankIndex >= banks.size()) {
                return false;
            }
            LoadedBank& loaded = loadedBanks[name.bankIndex];
            if (!loaded.loaded && !load_bank(source, banks[name.bankIndex], loaded)) {
                return false;
            }
            const ResolveStatus resolved = resolve_name(loaded, name.stringHash, name);
            if (resolved == ResolveStatus::invalid) {
                return false;
            }
            if (resolved == ResolveStatus::absent) {
                name.authoredEmpty = true;
                ++pending.authoredEmptyCount;
                continue;
            }
            ++pending.resolvedCount;
        }
        if (static_cast<std::size_t>(pending.resolvedCount) + pending.authoredEmptyCount
            != pending.names.size()) {
            return false;
        }
        output = std::move(pending);
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

/** Resolves the display name of every reference into the snapshot. */
bool resolve(const Source& source,
             std::span<const Reference> references,
             Snapshot& output) noexcept {
    output = {};
    if (source.read == nullptr || references.empty()) {
        return false;
    }
    try {
        Snapshot pending{};
        pending.names.resize(references.size());
        // Banks stay ordered by container tag, so a repeated container costs a search, not a read.
        std::vector<LoadedBankEntry> loaded{};
        for (std::size_t index = 0; index < references.size(); ++index) {
            const Reference& reference = references[index];
            if (reference.containerTag == 0 || reference.stringHash == kLiteralPartHash) {
                pending.names[index].authoredEmpty = true;
                ++pending.authoredEmptyCount;
                continue;
            }
            auto found = std::lower_bound(loaded.begin(),
                                          loaded.end(),
                                          reference.containerTag,
                                          [](const LoadedBankEntry& entry, std::uint32_t tag) {
                                              return entry.containerTag < tag;
                                          });
            if (found == loaded.end() || found->containerTag != reference.containerTag) {
                found = loaded.insert(found, LoadedBankEntry{reference.containerTag, {}});
                if (!load_bank(source, Bank{0, reference.containerTag}, found->bank)) {
                    pending.names[index].authoredEmpty = true;
                    ++pending.authoredEmptyCount;
                    continue;
                }
            }
            Name& name = pending.names[index];
            name.stringHash = reference.stringHash;
            const ResolveStatus status = resolve_name(found->bank, reference.stringHash, name);
            if (status == ResolveStatus::invalid) {
                name = {};
                name.stringHash = reference.stringHash;
                name.authoredEmpty = true;
                ++pending.authoredEmptyCount;
                continue;
            }
            if (status == ResolveStatus::resolved) {
                ++pending.resolvedCount;
            } else {
                name.authoredEmpty = true;
                ++pending.authoredEmptyCount;
            }
        }
        output = std::move(pending);
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

/** Reads each container's hash array once and keeps the first container for every wanted hash. */
bool locate(const Source& source,
            std::span<const std::uint32_t> containerTags,
            std::span<const std::uint32_t> stringHashes,
            std::vector<Reference>& output) noexcept {
    output.clear();
    // Ascending without repeats: no neighbour pair may be out of order or equal.
    if (source.read == nullptr
        || std::adjacent_find(stringHashes.begin(), stringHashes.end(), std::greater_equal<>{})
               != stringHashes.end()) {
        return false;
    }
    try {
        std::vector<std::uint32_t> tags(containerTags.begin(), containerTags.end());
        std::sort(tags.begin(), tags.end());
        tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
        // One owning container per wanted hash, zero until a container holding it is read.
        std::vector<std::uint32_t> owners(stringHashes.size(), 0U);
        std::size_t remaining = owners.size();
        std::vector<std::byte> container;
        for (const std::uint32_t tag : tags) {
            if (remaining == 0) {
                break;
            }
            Array hashes{};
            if (tag == 0 || !source.read(source.context, tag, kStringContainerClass, container)
                || !read_array(container,
                               kContainerHashFieldOffset,
                               activities::kBankHashStride,
                               kMaximumArrayRows,
                               kContainerHashElementClass,
                               hashes)) {
                continue;
            }
            for (std::size_t index = 0; index < hashes.count; ++index) {
                std::uint32_t hash = 0;
                if (!read(container, hashes.data + index * activities::kBankHashStride, hash)) {
                    break;
                }
                const auto found = std::lower_bound(stringHashes.begin(), stringHashes.end(), hash);
                if (found == stringHashes.end() || *found != hash) {
                    continue;
                }
                std::uint32_t& owner =
                    owners[static_cast<std::size_t>(found - stringHashes.begin())];
                if (owner == 0) {
                    owner = tag;
                    --remaining;
                }
            }
        }
        for (std::size_t index = 0; index < owners.size(); ++index) {
            if (owners[index] != 0) {
                output.push_back({owners[index], stringHashes[index]});
            }
        }
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

} // namespace sunrise::middleware::content::packages::tables::activity_display_names
