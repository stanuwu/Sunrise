#include "activity_definition_reader.h"

#include <cstddef>
#include <limits>
#include <type_traits>
#include <vector>

#include "../reader/layout.h"
#include "../schema/generated/tag_classes_808076.h"
#include "../schema/generated/tag_classes_808077.h"
#include "definition_index_table.h"
#include "internal.h"

namespace sunrise::middleware::content::packages::tables {
namespace {

namespace generated = schema::generated;
/** The index array's marker and header sit right before its rows. */
using Header = reader::layout::TypedArrayHeader;

using Row = generated::ActivityRecord;
using Record = generated::ActivityDefinition;

/** One index row holds a hash, zero padding, and a signed self-relative pointer. */
constexpr std::size_t kIndexStride = sizeof(Row);
constexpr std::size_t kIndexPaddingOffset = offsetof(Row, gap4);
constexpr std::size_t kIndexPointerOffset = offsetof(Row, activityPointer);
/** Element class of one index row. */
constexpr std::uint32_t kIndexRowClass = Row::kClass;
/** The table root ends here, so no index or record may start before it. */
constexpr std::size_t kTableRootSize = sizeof(generated::ActivityDefinitionTable);

// The shared table constants must match the game's reflection records.
static_assert(kActivityDefinitionTableClass == generated::ActivityDefinitionTable::kClass);
static_assert(
    std::is_same_v<decltype(generated::ActivityDefinitionTable::table), generated::TagArray<Row>>);
static_assert(std::is_same_v<decltype(Row::activityPointer), generated::RelativePointer<Record>>);
static_assert(kStockActivityDefinitionCount <= kActivityDefinitionMaximumCount);
/** Every fixed field used by the inventory ends with the destination byte. */
constexpr std::size_t kMinimumRecordSize =
    offsetof(Record, destination) + sizeof(Record::destination);

/** Activity record field offsets. */
constexpr std::size_t kRecordDefinitionHashOffset = offsetof(Record, definitionHash);
constexpr std::size_t kRecordMatchmakingPointerOffset = offsetof(Record, matchmakingConfig);
constexpr std::size_t kRecordInternalNamePointerOffset = offsetof(Record, internalName);
constexpr std::size_t kRecordRequiredLevelOffset = offsetof(Record, requiredLevel);
constexpr std::size_t kRecordRequiredPowerOffset = offsetof(Record, requiredPower);
constexpr std::size_t kRecordRequiredLevel2Offset = offsetof(Record, requiredLevel2);
constexpr std::size_t kRecordRequiredPower2Offset = offsetof(Record, requiredPower2);
constexpr std::size_t kRecordUnlockSlotOffset = offsetof(Record, unlockSlot);
constexpr std::size_t kRecordTypeIndexOffset = offsetof(Record, activityType);
constexpr std::size_t kRecordGameplaySettingsHashOffset = offsetof(Record, gameplaySettingsHash);
constexpr std::size_t kRecordDestinationIndexOffset = offsetof(Record, destination);

/** The installed type enum has this exclusive upper bound; mods add destinations, not types. */
constexpr std::uint8_t kTypeCount = 54;
/** Package tags use two 13-bit lanes above their base. */
constexpr std::uint64_t kTagNamespaceSize = std::uint64_t{1} << 26U;

/** One validated index row. */
struct IndexEntry final {
    std::uint32_t definitionHash{};
    std::size_t recordOffset{};
};

/** The validated index and the byte span it fills, marker included. */
struct Index final {
    std::size_t begin{};
    std::size_t end{};
    std::vector<IndexEntry> rows;
};

/** Resolves a signed pointer and requires its target field to fit in the blob. */
[[nodiscard]] bool follow(std::span<const std::byte> blob,
                          std::size_t member,
                          std::size_t targetSize,
                          std::size_t& target) noexcept {
    std::int64_t relative = 0;
    return read(blob, member, relative) && relative_offset(member, relative, target)
           && target <= blob.size() && targetSize <= blob.size() - target;
}

/** The package namespace has two 13-bit index lanes. */
[[nodiscard]] bool valid_package_tag(std::uint32_t tag) noexcept {
    return tag >= reader::layout::kTagBase
           && static_cast<std::uint64_t>(tag - reader::layout::kTagBase) < kTagNamespaceSize;
}

/** Reads one strict ASCII string whose NUL must occur inside the fixed scan limit. */
[[nodiscard]] bool read_internal_name(std::span<const std::byte> blob,
                                      std::size_t offset,
                                      ActivityDefinition& output) noexcept {
    if (offset >= blob.size()) {
        return false;
    }
    const std::size_t available = blob.size() - offset;
    const std::size_t limit =
        available < output.internalName.size() ? available : output.internalName.size();
    for (std::size_t length = 0; length < limit; ++length) {
        const auto value = std::to_integer<std::uint8_t>(blob[offset + length]);
        if (value == 0) {
            output.internalNameLength = static_cast<std::uint16_t>(length);
            return true;
        }
        if (value > 0x7FU) {
            return false;
        }
        output.internalName[length] = static_cast<char>(value);
    }
    return false;
}

/**
 * Reads and validates the index, whose row count the table declares.
 * A merged table carries its rebuilt index at the blob end, so the index is found by descriptor.
 * @param blob Whole table bytes.
 * @param output Receives every row and the span the index fills.
 * @return False when the index or any row is malformed, or storage cannot be had.
 */
[[nodiscard]] bool read_index(std::span<const std::byte> blob, Index& output) noexcept {
    Array array{};
    if (!find_array_at(blob, offsetof(generated::ActivityDefinitionTable, table), array)
        || array.elementClass != kIndexRowClass || array.count == 0
        || array.count > kActivityDefinitionMaximumCount
        || array.dataOffset < kTableRootSize + Header::kMarkerBack + Header::kSize
        || array.dataOffset > blob.size()
        || array.count > (blob.size() - array.dataOffset) / kIndexStride) {
        return false;
    }
    const auto count = static_cast<std::size_t>(array.count);
    try {
        output.rows.assign(count, IndexEntry{});
    } catch (...) {
        return false;
    }
    output.begin = array.dataOffset - Header::kSize - Header::kMarkerBack;
    output.end = array.dataOffset + count * kIndexStride;
    std::size_t previousOffset = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t offset = array.dataOffset + index * kIndexStride;
        std::uint32_t padding = 0;
        std::int64_t relative = 0;
        std::size_t recordOffset = 0;
        if (!read(blob, offset, output.rows[index].definitionHash)
            || !read(blob, offset + kIndexPaddingOffset, padding) || padding != 0
            || !read(blob, offset + kIndexPointerOffset, relative)
            || !relative_offset(offset + kIndexPointerOffset, relative, recordOffset)
            || recordOffset < kTableRootSize || recordOffset >= blob.size()
            || (recordOffset >= output.begin && recordOffset < output.end)
            || (index != 0 && recordOffset <= previousOffset)) {
            return false;
        }
        output.rows[index].recordOffset = recordOffset;
        previousOffset = recordOffset;
    }
    return true;
}

/** Reads one activity after the complete index has passed validation. */
[[nodiscard]] bool read_definition(std::span<const std::byte> blob,
                                   const Index& table,
                                   std::size_t index,
                                   ActivityDefinition& output) noexcept {
    output = {};
    const std::vector<IndexEntry>& rows = table.rows;
    const IndexEntry& row = rows[index];
    // A record ends at its successor, or at the blob end when last, and never runs into the index.
    std::size_t recordEnd = index + 1 < rows.size() ? rows[index + 1].recordOffset : blob.size();
    if (row.recordOffset < table.begin && table.begin < recordEnd) {
        recordEnd = table.begin;
    }
    const std::size_t recordLength = recordEnd - row.recordOffset;
    std::uint32_t recordHash = 0;
    std::size_t matchmakingOffset = 0;
    std::uint32_t matchmakingTag = 0;
    std::size_t internalNameOffset = 0;
    if (recordLength < kMinimumRecordSize
        || !read(blob, row.recordOffset + kRecordDefinitionHashOffset, recordHash)
        || recordHash != row.definitionHash
        || !follow(blob,
                   row.recordOffset + kRecordMatchmakingPointerOffset,
                   sizeof matchmakingTag,
                   matchmakingOffset)
        || !read(blob, matchmakingOffset, matchmakingTag)
        || (matchmakingTag != kActivityDefinitionNoMatchmakingConfig
            && !valid_package_tag(matchmakingTag))
        || !follow(blob,
                   row.recordOffset + kRecordInternalNamePointerOffset,
                   sizeof(std::byte),
                   internalNameOffset)) {
        return false;
    }

    output.activityIndex = static_cast<std::uint32_t>(index);
    output.definitionHash = row.definitionHash;
    output.recordOffset = row.recordOffset;
    output.recordLength = recordLength;
    output.matchmakingPointerOffset = matchmakingOffset;
    output.matchmakingConfigTag = matchmakingTag;
    output.hasMatchmakingConfig = matchmakingTag != kActivityDefinitionNoMatchmakingConfig;
    output.internalNamePointerOffset = internalNameOffset;
    if (!read_internal_name(blob, internalNameOffset, output)
        || !read(blob, row.recordOffset + kRecordRequiredLevelOffset, output.requiredLevel)
        || !read(blob, row.recordOffset + kRecordRequiredPowerOffset, output.requiredPower)
        || !read(blob, row.recordOffset + kRecordRequiredLevel2Offset, output.requiredLevel2)
        || !read(blob, row.recordOffset + kRecordRequiredPower2Offset, output.requiredPower2)
        || !read(blob, row.recordOffset + kRecordUnlockSlotOffset, output.unlockSlot)
        || !read(blob, row.recordOffset + kRecordTypeIndexOffset, output.typeIndex)
        || !read(
            blob, row.recordOffset + kRecordGameplaySettingsHashOffset, output.gameplaySettingsHash)
        || !read(blob, row.recordOffset + kRecordDestinationIndexOffset, output.destinationIndex)
        || output.typeIndex >= kTypeCount) {
        output = {};
        return false;
    }
    return true;
}

} // namespace

/** Validates and visits the complete activity-definition table. */
bool visit_activity_definitions(std::span<const std::byte> blob,
                                ActivityDefinitionVisitor visitor,
                                void* context) noexcept {
    if (visitor == nullptr) {
        return false;
    }
    Index table{};
    if (!read_index(blob, table)) {
        return false;
    }

    ActivityDefinition definition{};
    for (std::size_t index = 0; index < table.rows.size(); ++index) {
        if (!read_definition(blob, table, index, definition)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < table.rows.size(); ++index) {
        if (!read_definition(blob, table, index, definition) || !visitor(context, definition)) {
            return false;
        }
    }
    return true;
}

} // namespace sunrise::middleware::content::packages::tables
