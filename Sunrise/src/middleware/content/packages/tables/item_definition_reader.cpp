#include <algorithm>
#include <cstddef>

#include "../../../../state/build_data/items/quest_initialization.h"
#include "../reader/typed_array.h"
#include "../schema/generated/tag_classes_80802d.h"
#include "../schema/generated/tag_classes_80802e.h"
#include "../schema/generated/tag_classes_808030.h"
#include "../schema/generated/tag_classes_808077.h"
#include "../schema/generated/tag_classes_80807b.h"
#include "../schema/generated/tag_classes_80807c.h"
#include "../schema/generated/tag_classes_80809f.h"
#include "definition_index_table.h"
#include "internal.h"
#include "items.h"

namespace sunrise::middleware::content::packages::tables::items {
namespace {

namespace generated = schema::generated;

/** The signed stack limit comes before the bucket id. */
constexpr std::size_t kMaxStackSizeOffset =
    offsetof(generated::ItemDefinition, inventoryItemBlock.maximumStack);
/** A nonzero predicate byte marks an instanced definition. */
constexpr std::size_t kInstancedOffset =
    offsetof(generated::ItemDefinition, inventoryItemBlock.instanced);
/** Item tier: 1 common through 5 exotic, and 0 for rows outside the ladder such as currencies. */
constexpr std::size_t kTierOffset = offsetof(generated::ItemDefinition, inventoryItemBlock.tier);
/**
 * The manifest's stack-unique label, as the FNV-1 hash of its string. Unnamed in the generated
 * layout; 12,922 of the 15,470 shipped items hold the empty string's hash here.
 */
constexpr std::size_t kStackUniqueLabelOffset =
    offsetof(generated::ItemDefinition, inventoryItemBlock.unk0);
/**
 * The manifest's recovery bucket as a runtime bucket id, or all bits set when it has none. The
 * unnamed byte after the bucket; shipped items name the 21-row FIFO bucket 34, 28, 37 or none.
 */
constexpr std::size_t kRecoveryBucketOffset =
    offsetof(generated::ItemDefinition, inventoryItemBlock.gap9);
/**
 * The second and third bytes after the instanced predicate hold 0 or 1. The client's change-record
 * writer turns them into the policy bits that skip the acquisition toast and count, or only the
 * toast. Unnamed in the generated layout; 680 shipped items set the first and 2 the second.
 */
constexpr std::size_t kQuietAcquisitionOffset =
    offsetof(generated::ItemDefinition, inventoryItemBlock.gapC) + 1;
constexpr std::size_t kToastlessAcquisitionOffset = kQuietAcquisitionOffset + 1;
// Pins the offsets the unnamed fields above were measured at against a regenerated layout.
static_assert(kStackUniqueLabelOffset == 176 && kRecoveryBucketOffset == 185
              && kQuietAcquisitionOffset == 189);
/** The equipment block is self-relative from this offset, zero when absent. */
constexpr std::size_t kEquipmentBlockOffset =
    offsetof(generated::ItemDefinition, equippingItemBlock);
/** The slot is read as an i32 over the u16 field and two gap bytes; nonzero gap means no slot. */
constexpr std::size_t kEquipmentSlotOffset = offsetof(generated::ItemEquippingBlock, equipmentSlot);
static_assert(kEquipmentSlotOffset + sizeof(std::int32_t) <= sizeof(generated::ItemEquippingBlock));
/** The socket entry list block is self-relative from this offset, zero when absent. */
constexpr std::size_t kSocketEntryListBlockOffset =
    offsetof(generated::ItemDefinition, talentGridItemBlock);
/** A present block holds the two-byte list index. A shorter tail is not a block. */
constexpr std::size_t kSocketEntryListBlockSize = sizeof(generated::ItemTalentGridBlock);
constexpr std::size_t kSocketEntryListIndexOffset =
    offsetof(generated::ItemTalentGridBlock, socketEntryListIndex);
/** Equipment slots run from 0 through 19. */
constexpr std::int32_t kEquipmentSlotCount = 20;
/** A socket entry stores its type index first and its initial plug next. */
constexpr std::size_t kSocketTypeOffset = offsetof(generated::ItemOrdinarySocketEntry, socketType);
constexpr std::size_t kSocketPlugOffset =
    offsetof(generated::ItemOrdinarySocketEntry, initialPlugItem);
/** Fixed fields end after the acquisition presentation bytes; every installed item reaches it. */
constexpr std::size_t kFixedFieldEnd = kToastlessAcquisitionOffset + 1;
/** The plug block is self-relative from this offset, zero when absent. */
constexpr std::size_t kPlugBlockOffset = offsetof(generated::ItemDefinition, plugItemBlock);
/** Plug category, used to expand three native reusable plug families, and server-roll set. */
constexpr std::size_t kPlugBlockCategoryOffset = offsetof(generated::PlugBlock, plugCategory);
constexpr std::size_t kPlugBlockRollSetOffset = offsetof(generated::PlugBlock, rollSetOrdinal);
/** A plug that grants another plug's effect carries one linked-plug row in this array. */
constexpr std::size_t kLinkedPlugArrayOffset = offsetof(generated::PlugBlock, hE0b00974);
constexpr std::uint32_t kLinkedPlugClass = generated::LinkedPlugBlock::kClass;
constexpr std::size_t kLinkedPlugIndexOffset =
    offsetof(generated::LinkedPlugBlock, linkedPlugIndex);
/** A roll-set ordinal outside the ladder: none, or the marker for a plug rolled by no set. */
constexpr std::uint16_t kNoRollSet = 0;
/** Embedded reusable-list array descriptor inside one ordinary socket entry. */
constexpr std::size_t kEmbeddedPlugListOffset =
    offsetof(generated::ItemOrdinarySocketEntry, embeddedPlugs);
/** Reusable and randomized shared plug-set row indices inside one socket entry. */
constexpr std::size_t kReusablePlugSetIndexOffset =
    offsetof(generated::ItemOrdinarySocketEntry, reusablePlugSetRow);
constexpr std::size_t kRandomizedPlugSetIndexOffset =
    offsetof(generated::ItemOrdinarySocketEntry, randomizedPlugSetRow);
/** One shared plug-set table row and its member descriptor. */
constexpr std::size_t kPlugSetRowStride = sizeof(generated::SPlugSetRow);
constexpr std::size_t kPlugSetMemberDescriptorOffset = offsetof(generated::SPlugSetRow, entries);
/** Both embedded and shared plug member rows name an item index first. */
constexpr std::size_t kPlugMemberStride = sizeof(generated::PlugSetMember);
constexpr std::size_t kPlugMemberIndexOffset = offsetof(generated::PlugSetMember, plugItemIndex);
/** Native arrays use 16-bit definition indices even though their serialized field is 32-bit. */
constexpr std::uint64_t kMaximumPlugMemberCount = 65535;
/**
 * The schema leaves the objective block's completion flags unnamed in gap10: the all-objectives
 * byte at block offset 0x17 and the required-objectives byte after it.
 */
constexpr std::size_t kRequireAllObjectivesOffset =
    offsetof(generated::ItemObjectiveBlock, gap10) + 7;
constexpr std::size_t kRequireNonOptionalCompletionOffset = kRequireAllObjectivesOffset + 1;
static_assert(kRequireAllObjectivesOffset == 0x17);
/** A bounty declares its lifetime as a one-row expression array in its inventory block. */
constexpr std::size_t kLifetimeExpressionField =
    offsetof(generated::ItemDefinition, inventoryItemBlock)
    + offsetof(generated::C808077f5, h34f70b23);

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
    if (read(definition, static_cast<std::size_t>(block) + kSocketEntryListIndexOffset, index)) {
        row.hasSocketEntryList = true;
        row.socketEntryListIndex = index;
    }
}

/**
 * Reads the optional equipment slot a definition declares.
 * @param definition Whole item definition bytes.
 * @param slot Receives the slot when the block is present and readable.
 * @param raw Receives the unvalidated declared value, or -1 when the block is absent.
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
    if (!read(definition, static_cast<std::size_t>(block) + kEquipmentSlotOffset, value)) {
        return;
    }
    raw = value;
    if (value >= 0 && value < static_cast<std::int32_t>(kEquipmentSlotCount)) {
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

/** Walks one checked array of 32-byte plug rows. */
[[nodiscard]] bool visit_plug_array(std::span<const std::byte> blob,
                                    const Array& array,
                                    AllowedPlugVisitor visitor,
                                    void* context) noexcept {
    if (array.count > kMaximumPlugMemberCount || array.dataOffset > blob.size()
        || array.count > (blob.size() - array.dataOffset) / kPlugMemberStride) {
        return false;
    }
    for (std::uint64_t index = 0; index < array.count; ++index) {
        std::uint32_t itemDefinitionIndex = 0;
        const std::size_t row =
            array.dataOffset + static_cast<std::size_t>(index) * kPlugMemberStride;
        if (!read(blob, row + kPlugMemberIndexOffset, itemDefinitionIndex)
            || !visitor(context, itemDefinitionIndex)) {
            return false;
        }
    }
    return true;
}

/** Walks one reusable or randomized shared plug-set row, when the socket declares it. */
[[nodiscard]] bool visit_shared_plug_set(std::span<const std::byte> definition,
                                         std::size_t socketEntry,
                                         std::size_t setIndexOffset,
                                         std::span<const std::byte> plugSetTable,
                                         const Array& sets,
                                         AllowedPlugVisitor visitor,
                                         void* context) noexcept {
    std::uint16_t setIndex = kUnavailablePlug;
    if (!read(definition, socketEntry + setIndexOffset, setIndex)) {
        return false;
    }
    if (setIndex == kUnavailablePlug) {
        return true;
    }
    if (setIndex >= sets.count || sets.dataOffset > plugSetTable.size()
        || sets.count > (plugSetTable.size() - sets.dataOffset) / kPlugSetRowStride) {
        return false;
    }
    const std::size_t descriptor = sets.dataOffset
                                   + static_cast<std::size_t>(setIndex) * kPlugSetRowStride
                                   + kPlugSetMemberDescriptorOffset;
    std::uint64_t memberCount = 0;
    if (!read(plugSetTable, descriptor, memberCount)) {
        return false;
    }
    if (memberCount == 0) {
        return true;
    }
    Array members{};
    return find_array_at(plugSetTable, descriptor, members)
           && visit_plug_array(plugSetTable, members, visitor, context);
}

/** The block's stat array: its count, then a self-relative pointer to the array header. */
constexpr std::size_t kStatCountMember =
    offsetof(generated::SItemSandboxPerkBlock, itemStats.count);
constexpr std::size_t kStatDataMember =
    offsetof(generated::SItemSandboxPerkBlock, itemStats.relative);
/** One stat entry. */
constexpr std::size_t kStatEntryStride = sizeof(generated::SItemStatRow);
constexpr std::size_t kStatEntryRow = offsetof(generated::SItemStatRow, itemStat.statIndex);
/** A stat entry carries its signed contribution here. */
constexpr std::size_t kStatEntryValue = offsetof(generated::SItemStatRow, itemStat.value);

/**
 * Reads the plug block the definition points to: category, roll set, requirement sets and any
 * linked plug. A definition with no plug block keeps the row's absent values.
 * @param definition Whole item definition bytes.
 * @param row Receives the plug fields.
 */
void read_plug_block(std::span<const std::byte> definition, Row& row) noexcept {
    row.rollSetIndex = kNoRollSet;
    row.linkedPlugIndex = kUnavailablePlug;
    std::int64_t relative = 0;
    if (!read(definition, kPlugBlockOffset, relative) || relative == 0) {
        return;
    }
    const std::int64_t signedBlock = static_cast<std::int64_t>(kPlugBlockOffset) + relative;
    if (signedBlock < 0) {
        return;
    }
    const auto block = static_cast<std::size_t>(signedBlock);
    (void)read(definition, block + kPlugBlockCategoryOffset, row.plugCategoryHash);
    (void)read(definition, block + kPlugBlockRollSetOffset, row.rollSetIndex);
    (void)read(definition,
               block + kInsertionMaterialRequirementSetIndexOffset,
               row.insertionMaterialRequirementSetIndex);
    (void)read(definition,
               block + kEnabledMaterialRequirementSetIndexOffset,
               row.enabledMaterialRequirementSetIndex);
    Array linked{};
    if (find_array_at(definition, block + kLinkedPlugArrayOffset, linked)
        && linked.elementClass == kLinkedPlugClass) {
        (void)read(definition, linked.dataOffset + kLinkedPlugIndexOffset, row.linkedPlugIndex);
    }
}

/**
 * Reads the investment stat block into the row.
 * Two self-relative hops, valid only against the serialized definition.
 * @param definition Whole item definition record.
 * @param row Receives the stats, left empty when the block is absent or malformed.
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
    if (!read(definition, static_cast<std::size_t>(header) + kStatCountMember, count) || count == 0
        || count > kStatCapacity
        || !read(definition, static_cast<std::size_t>(header) + kStatDataMember, dataRelative)) {
        return;
    }
    const std::int64_t data = header + static_cast<std::int64_t>(kStatDataMember) + dataRelative;
    reader::TypedArrayRows rows{};
    if (data < 0
        || !reader::read_typed_array(definition,
                                     static_cast<std::size_t>(data),
                                     count,
                                     reader::kAnyElementClass,
                                     kStatEntryStride,
                                     rows)) {
        return;
    }

    for (std::uint64_t entry = 0; entry < count; ++entry) {
        const auto base = rows.data + kStatEntryStride * entry;
        if (!read(definition, base + kStatEntryRow, row.statRows[entry])
            || !read(definition, base + kStatEntryValue, row.statValues[entry])) {
            row.statCount = 0;
            return;
        }
    }
    row.statCount = static_cast<std::uint8_t>(count);
}

/** Reads the completion controls of one objective block. */
[[nodiscard]] bool read_completion_metadata(std::span<const std::byte> definition,
                                            std::size_t block,
                                            Row& row) noexcept {
    if (!read_boolean(definition, block + kRequireAllObjectivesOffset, row.requireAllObjectives)
        || !read_boolean(definition,
                         block + kRequireNonOptionalCompletionOffset,
                         row.requireNonOptionalCompletion)) {
        return false;
    }
    row.hasCompletionMetadata = true;
    return true;
}

/**
 * A bounty's lifetime expression is one literal row whose operand is the lifetime in seconds. Only
 * a literal states the lifetime, so any other expression leaves the block unreadable.
 */
[[nodiscard]] bool read_lifetime(std::span<const std::byte> definition, Row& row) noexcept {
    Array lifetime{};
    if (!find_optional_array_at(definition, kLifetimeExpressionField, lifetime)) {
        return false;
    }
    if (lifetime.count == 0) {
        return true;
    }
    std::uint32_t opcode = 0;
    std::int32_t seconds = 0;
    if (lifetime.count != 1 || lifetime.elementClass != kInvestmentExpressionRowClass
        || !read(definition, lifetime.dataOffset, opcode) || opcode != kUnlockLiteralOpcode
        || !read(definition, lifetime.dataOffset + kUnlockInstructionOperandOffset, seconds)
        || seconds < 0) {
        return false;
    }
    row.lifetimeSeconds = seconds;
    return true;
}

/** Reads a pursuit's objective block, completion controls and lifetime. */
[[nodiscard]] bool read_pursuit_metadata(std::span<const std::byte> definition, Row& row) noexcept {
    // Other definitions reuse this field for other block types, equipment among them.
    if (row.bucketId != state::build_data::items::kPursuitBucketId || row.equipmentSlot.has_value()
        || row.maxStackSize > 1) {
        return true;
    }
    std::size_t block = 0;
    bool present = false;
    if (!class_block(
            definition, kItemObjectiveBlockOffset, kItemObjectiveBlockClass, block, present)) {
        return false;
    }
    if (!present) {
        return true;
    }
    Array objectives{};
    if (!read_completion_metadata(definition, block, row)
        || !find_optional_array_at(definition, block, objectives)) {
        return false;
    }
    // An empty objective list may carry no array header. Its lifetime is read all the same.
    if (objectives.count != 0
        && (objectives.elementClass != kObjectiveReferenceArrayClass
            || objectives.count > state::build_data::pursuits::kObjectiveCapacity)) {
        return false;
    }
    for (std::size_t i = 0; i < objectives.count; ++i) {
        if (!read(definition,
                  objectives.dataOffset + i * kObjectiveReferenceStride,
                  row.objectiveIndices[i])) {
            return false;
        }
    }
    row.objectiveCount = static_cast<std::uint8_t>(objectives.count);
    return read_lifetime(definition, row);
}

} // namespace

/** Reads the fixed fields of one item definition blob. */
bool read_definition(std::span<const std::byte> definition, Row& row) noexcept {
    const std::uint32_t hash = row.definitionHash;
    const std::uint16_t index = row.definitionIndex;
    row = {};
    row.definitionHash = hash;
    row.definitionIndex = index;
    row.insertionMaterialRequirementSetIndex = kUnavailableMaterialRequirementSetIndex;
    row.enabledMaterialRequirementSetIndex = kUnavailableMaterialRequirementSetIndex;
    std::fill(std::begin(row.initialPlugs), std::end(row.initialPlugs), kUnavailablePlug);
    std::fill(std::begin(row.socketTypes), std::end(row.socketTypes), kUnavailableSocketType);
    if (definition.size() < kFixedFieldEnd) {
        return false;
    }
    std::uint8_t instanced = 0;
    std::uint8_t quietAcquisition = 0;
    std::uint8_t toastlessAcquisition = 0;
    if (!read(definition, kBucketIdOffset, row.bucketId)
        || !read(definition, kMaxStackSizeOffset, row.maxStackSize)
        || !read(definition, kTierOffset, row.tier)
        || !read(definition, kInstancedOffset, instanced)
        || !read(definition, kRecoveryBucketOffset, row.recoveryBucketId)
        || !read(definition, kStackUniqueLabelOffset, row.stackUniqueLabel)
        || !read(definition, kQuietAcquisitionOffset, quietAcquisition)
        || !read(definition, kToastlessAcquisitionOffset, toastlessAcquisition)
        || quietAcquisition > 1 || toastlessAcquisition > 1) {
        return false;
    }
    row.instanced = instanced != 0;
    row.quietAcquisition = quietAcquisition != 0;
    row.toastlessAcquisition = toastlessAcquisition != 0;
    read_plug_block(definition, row);
    read_stats(definition, row);
    read_appearance(definition, row);
    read_socket_entry_list(definition, row);
    read_equipment_slot(definition, row.equipmentSlot, row.rawEquipmentSlot);
    row.objectiveMetadataValid = read_pursuit_metadata(definition, row);
    read_sockets(definition, row);
    return true;
}

/** Visits every list-backed allowed plug for one exact ordinary socket lane. */
bool visit_allowed_plugs(std::span<const std::byte> definition,
                         std::span<const std::byte> plugSetTable,
                         std::uint8_t lane,
                         AllowedPlugVisitor visitor,
                         void* context) noexcept {
    if (visitor == nullptr || lane >= kSocketCapacity) {
        return false;
    }
    std::int64_t socketBlockRelative = 0;
    if (!read(definition, kSocketBlockOffset, socketBlockRelative) || socketBlockRelative == 0) {
        return false;
    }
    const std::int64_t socketBlock =
        static_cast<std::int64_t>(kSocketBlockOffset) + socketBlockRelative;
    if (socketBlock < 0 || static_cast<std::uint64_t>(socketBlock) >= definition.size()) {
        return false;
    }
    Array sockets{};
    if (!find_array_at(definition, static_cast<std::size_t>(socketBlock), sockets)
        || sockets.elementClass != kOrdinarySocketClass || lane >= sockets.count
        || sockets.dataOffset > definition.size()
        || sockets.count > (definition.size() - sockets.dataOffset) / kSocketEntryStride) {
        return false;
    }
    Array sets{};
    if (!find_array_at(plugSetTable, offsetof(generated::C80802cb6, table), sets)) {
        return false;
    }
    const std::size_t socketEntry =
        sockets.dataOffset + static_cast<std::size_t>(lane) * kSocketEntryStride;
    Array embedded{};
    std::uint64_t embeddedCount = 0;
    if (!read(definition, socketEntry + kEmbeddedPlugListOffset, embeddedCount)
        || (embeddedCount != 0
            && (!find_array_at(definition, socketEntry + kEmbeddedPlugListOffset, embedded)
                || !visit_plug_array(definition, embedded, visitor, context)))) {
        return false;
    }
    return visit_shared_plug_set(definition,
                                 socketEntry,
                                 kReusablePlugSetIndexOffset,
                                 plugSetTable,
                                 sets,
                                 visitor,
                                 context)
           && visit_shared_plug_set(definition,
                                    socketEntry,
                                    kRandomizedPlugSetIndexOffset,
                                    plugSetTable,
                                    sets,
                                    visitor,
                                    context);
}

} // namespace sunrise::middleware::content::packages::tables::items
