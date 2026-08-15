#include <algorithm>
#include <cstring>

#include "definition_index_table.h"
#include "items.h"

namespace sunrise::middleware::content::packages::tables::items {
namespace {

/** The signed stack limit comes before the bucket id. */
constexpr std::size_t kMaxStackSizeOffset = 180;
/** A nonzero predicate byte marks an instanced definition. */
constexpr std::size_t kInstancedOffset = 187;
/** The equipment block is self-relative from this offset, zero when absent. */
constexpr std::size_t kEquipmentBlockOffset = 16;
/** The equipment block stores its signed slot id here. */
constexpr std::size_t kEquipmentSlotOffset = 24;
/** The socket entry list block is self-relative from this offset, zero when absent. */
constexpr std::size_t kSocketEntryListBlockOffset = 128;
/** A present block declares 12 bytes. A shorter tail is not a block. */
constexpr std::size_t kSocketEntryListBlockSize = 12;
/** Equipment slots run from 0 through 19. */
constexpr std::int32_t kEquipmentSlotCount = 20;
/** A socket entry stores its type index first and its initial plug next. */
constexpr std::size_t kSocketTypeOffset = 0;
constexpr std::size_t kSocketPlugOffset = 2;
/** Fixed fields end after the instanced predicate. */
constexpr std::size_t kFixedFieldEnd = kInstancedOffset + 1;

/** @param blob Source bytes. @param offset Field offset. @param value Receives the field. */
template <typename Value>
[[nodiscard]] bool
read(std::span<const std::byte> blob, std::size_t offset, Value& value) noexcept {
    if (offset > blob.size() || blob.size() - offset < sizeof value) {
        return false;
    }
    std::memcpy(&value, blob.data() + offset, sizeof value);
    return true;
}

/**
 * Reads the socket entry list index a definition declares.
 * @param definition Whole item definition bytes.
 * @param row Receives list presence and index.
 */
void read_socket_entry_list(std::span<const std::byte> definition, Row& row) noexcept {
    row.hasSocketEntryList = false;
    row.socketEntryListIndex = 0;
    std::int64_t relative = 0;
    if (!read(definition, kSocketEntryListBlockOffset, relative) || relative == 0) {
        return;
    }
    const std::int64_t block = static_cast<std::int64_t>(kSocketEntryListBlockOffset) + relative;
    if (block < 0) {
        return;
    }
    const auto blockOffset = static_cast<std::uint64_t>(block);
    if (blockOffset + kSocketEntryListBlockSize > definition.size()) {
        return;
    }
    std::uint16_t index = 0;
    if (read(definition, static_cast<std::size_t>(block), index)) {
        row.hasSocketEntryList = true;
        row.socketEntryListIndex = index;
    }
}

/**
 * Reads the optional equipment slot a definition declares.
 * @param definition Whole item definition bytes.
 * @param slot Receives the slot when the block is present and readable.
 */
void read_equipment_slot(std::span<const std::byte> definition,
                         std::optional<std::int8_t>& slot,
                         std::int32_t& raw) noexcept {
    slot.reset();
    raw = -1;
    std::int64_t relative = 0;
    if (!read(definition, kEquipmentBlockOffset, relative) || relative == 0) {
        return;
    }
    const std::int64_t block = static_cast<std::int64_t>(kEquipmentBlockOffset) + relative;
    if (block < 0) {
        return;
    }
    // A value outside the equippable range marks an item that takes no slot.
    std::int32_t value = 0;
    if (read(definition, static_cast<std::size_t>(block) + kEquipmentSlotOffset, value)
        && value >= 0 && value < static_cast<std::int32_t>(kEquipmentSlotCount)) {
        slot = static_cast<std::int8_t>(value);
    }
}

/**
 * Reads the ordinary socket lanes a definition declares.
 * The lane's type index picks the overflow-hash priority the character record applies, so it is
 * read from the same entry as the plug rather than worked out later.
 * @param definition Whole item definition bytes.
 * @param row Receives socket presence, count, types and initial plugs.
 */
void read_sockets(std::span<const std::byte> definition, Row& row) noexcept {
    row.hasSockets = false;
    row.socketCount = 0;
    std::fill(std::begin(row.initialPlugs), std::end(row.initialPlugs), kUnavailablePlug);
    std::fill(std::begin(row.socketTypes), std::end(row.socketTypes), kUnavailableSocketType);
    std::int64_t relative = 0;
    if (!read(definition, kSocketBlockOffset, relative) || relative == 0) {
        return;
    }
    const std::int64_t block = static_cast<std::int64_t>(kSocketBlockOffset) + relative;
    if (block < 0 || static_cast<std::uint64_t>(block) >= definition.size()) {
        return;
    }
    Array array{};
    if (!find_array_at(definition, static_cast<std::size_t>(block), array)
        || array.elementClass != kOrdinarySocketClass) {
        return;
    }
    row.hasSockets = true;
    const auto lanes =
        static_cast<std::size_t>((std::min)(array.count, std::uint64_t{kSocketCapacity}));
    for (std::size_t lane = 0; lane < lanes; ++lane) {
        const std::size_t entry = array.dataOffset + lane * kSocketEntryStride;
        std::uint16_t plug = kUnavailablePlug;
        std::uint16_t type = kUnavailableSocketType;
        if (!read(definition, entry + kSocketPlugOffset, plug)
            || !read(definition, entry + kSocketTypeOffset, type)) {
            return;
        }
        row.initialPlugs[lane] = plug;
        row.socketTypes[lane] = type;
        row.socketCount = static_cast<std::uint8_t>(lane + 1);
    }
}

/** The block header carries its own self-relative pointer to the entries at byte 8. */
constexpr std::size_t kStatDataMember = 8;
/** One stat entry is 40 blob bytes. */
constexpr std::size_t kStatEntryStride = 40;
/** A stat entry names its stat table row at byte 16. */
constexpr std::size_t kStatEntryRow = 16;
/** A stat entry carries its signed contribution at byte 20. */
constexpr std::size_t kStatEntryValue = 20;

/**
 * Reads the per-item stat contributions declared inside the definition blob.
 * Two self-relative hops: the definition points at a header, the header points at the entries.
 * Both are blob offsets and are valid only against the serialized definition.
 * @param definition Whole item definition bytes.
 * @param row Receives the declared stat rows and values.
 */
void read_stats(std::span<const std::byte> definition, Row& row) noexcept {
    row.statCount = 0;
    std::int64_t blockRelative = 0;
    if (!read(definition, kStatBlockOffset, blockRelative)) {
        return;
    }
    const auto header = static_cast<std::int64_t>(kStatBlockOffset) + blockRelative;
    if (header < 0 || static_cast<std::size_t>(header) + kStatDataMember > definition.size()) {
        return;
    }

    std::uint64_t count = 0;
    std::int64_t dataRelative = 0;
    if (!read(definition, static_cast<std::size_t>(header), count) || count == 0
        || count > kStatCapacity
        || !read(definition, static_cast<std::size_t>(header) + kStatDataMember, dataRelative)) {
        return;
    }
    const std::int64_t data = header + static_cast<std::int64_t>(kStatDataMember) + dataRelative;
    if (data < 0 || static_cast<std::size_t>(data) + kStatEntryStride * count > definition.size()) {
        return;
    }

    for (std::uint64_t entry = 0; entry < count; ++entry) {
        const auto base = static_cast<std::size_t>(data) + kStatEntryStride * entry;
        if (!read(definition, base + kStatEntryRow, row.statRows[entry])
            || !read(definition, base + kStatEntryValue, row.statValues[entry])) {
            row.statCount = 0;
            return;
        }
    }
    row.statCount = static_cast<std::uint8_t>(count);
}

} // namespace

/** Reads the fixed fields of one item definition blob. */
bool read_definition(std::span<const std::byte> definition, Row& row) noexcept {
    const std::uint32_t hash = row.definitionHash;
    const std::uint16_t index = row.definitionIndex;
    row = {};
    row.definitionHash = hash;
    row.definitionIndex = index;
    std::fill(std::begin(row.initialPlugs), std::end(row.initialPlugs), kUnavailablePlug);
    std::fill(std::begin(row.socketTypes), std::end(row.socketTypes), kUnavailableSocketType);
    if (definition.size() < kFixedFieldEnd) {
        return false;
    }
    std::uint8_t instanced = 0;
    if (!read(definition, kBucketIdOffset, row.bucketId)
        || !read(definition, kMaxStackSizeOffset, row.maxStackSize)
        || !read(definition, kInstancedOffset, instanced)) {
        return false;
    }
    row.instanced = instanced != 0;
    read_stats(definition, row);
    read_appearance(definition, row);
    read_socket_entry_list(definition, row);
    read_equipment_slot(definition, row.equipmentSlot, row.rawEquipmentSlot);
    read_sockets(definition, row);
    return true;
}

} // namespace sunrise::middleware::content::packages::tables::items
