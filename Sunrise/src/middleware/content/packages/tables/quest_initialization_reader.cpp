#include "quest_initialization_reader.h"

#include <algorithm>
#include <cstddef>

#include "../schema/generated/tag_classes_808077.h"
#include "../schema/generated/tag_classes_80807b.h"
#include "../schema/generated/tag_classes_80807c.h"
#include "../schema/generated/tag_classes_80807d.h"
#include "definition_index_table.h"
#include "internal.h"

namespace sunrise::middleware::content::packages::tables::items {
namespace {

namespace generated = schema::generated;
using Quest = state::build_data::items::QuestInitialization;
using QuestStep = state::build_data::pursuits::QuestStep;

/** Policy: shorter item blobs stay outside the supported quest layout. */
constexpr std::size_t kMinimumQuestDefinitionSize = sizeof(generated::ItemDefinition);
/** Policy: every quest block keeps at least the smallest quest block's fixed part. */
constexpr std::size_t kMinimumQuestBlockSize = (std::min)({sizeof(generated::ItemObjectiveBlock),
                                                           sizeof(generated::QuestSetBlock),
                                                           sizeof(generated::ItemUnlockBlock)});

/** Quest-set block offset, relative to the field. */
constexpr std::size_t kItemQuestSetBlockOffset = offsetof(generated::ItemDefinition, setItemBlock);
/** This block holds ordered quest members and the value slot that selects the active step. */
constexpr std::uint32_t kQuestSetBlockClass = generated::QuestSetBlock::kClass;
constexpr std::size_t kQuestSetMemberDescriptor = offsetof(generated::QuestSetBlock, itemList);
/** A 16-bit unlock value slot; a map supplies its saved bank row. */
constexpr std::size_t kQuestSetValueSlotOffset = offsetof(generated::QuestSetBlock, valueSlot);
/** A one-byte mode separate from the member values. */
constexpr std::size_t kQuestSetModeOffset = offsetof(generated::QuestSetBlock, mode);
/** Policy: only mode 1 permits first-step writes; the other modes are not decoded. */
constexpr std::uint8_t kSupportedQuestSetMode = 1;
/** Each member pairs a signed step value with the item that represents it. */
constexpr std::uint32_t kQuestSetMemberClass = generated::QuestSetMember::kClass;
/** A member is a 32-bit value, a 16-bit item index, then a 16-bit reserved field. */
constexpr std::size_t kQuestSetMemberStride = sizeof(generated::QuestSetMember);
/** The signed step identifier; values are not ordered progress counts. */
constexpr std::size_t kQuestSetMemberValueOffset = offsetof(generated::QuestSetMember, stepValue);
/** The item-table index for that step. */
constexpr std::size_t kQuestSetMemberItemOffset = offsetof(generated::QuestSetMember, itemIndex);
/** Only member rows whose final 16 bits are zero are supported. */
constexpr std::size_t kQuestSetMemberReservedOffset = offsetof(generated::QuestSetMember, reserved);

/** Authored value/flag slots must fit the nonnegative range of a signed 16-bit mapping. */
constexpr std::uint16_t kUnlockSlotLimit = 0x8000U;
/** Policy: pursuits without presence flags need a bucket-37 root with no objective block. */
constexpr std::uint8_t kSeparateQuestRootBucketId = 37;

/** The third map has no supported save bank; a matching slot makes initialization unsafe. */
constexpr std::size_t kThirdValueMapDescriptor = offsetof(generated::C80807c80, h201ada27);
/** The fourth map is also checked for duplicate slots but has no supported quest bank. */
constexpr std::size_t kFourthValueMapDescriptor = kCharacterBlockValueMapDescriptor;
/** A matching map row is supported only when its final 16 bits are zero. */
constexpr std::size_t kValueMapReservedOffset = offsetof(generated::UnlockValueMapRow, reserved);
/** The all-one 16-bit row is reserved and cannot name saved state. */
constexpr std::uint16_t kUnavailableValueMapRow = 0xFFFFU;

/**
 * A block pointer is relative to its own field; its class ID precedes the payload.
 * @param bytes Blob containing the pointer and block.
 * @param field Offset of the signed 64-bit block pointer.
 * @param expectedClass Required serialized block class.
 * @param offset Receives the payload offset; use only on success.
 * @return False for absent, out-of-bounds, short, or wrong-class blocks.
 */
[[nodiscard]] bool block(std::span<const std::byte> bytes,
                         std::size_t field,
                         std::uint32_t expectedClass,
                         std::size_t& offset) noexcept {
    bool present = false;
    return class_block(bytes, field, expectedClass, offset, present) && present
           && bytes.size() - offset >= kMinimumQuestBlockSize;
}

/**
 * The whole fixed-stride array must fit the blob before any row is read.
 * @param bytes Blob containing the descriptor and rows.
 * @param field Offset of the array descriptor.
 * @param expectedClass Required serialized element class.
 * @param stride Nonzero byte width of one row.
 * @param rows Receives the array bounds; use only on success.
 * @return False when the descriptor, class, or row bounds are invalid.
 */
[[nodiscard]] bool array(std::span<const std::byte> bytes,
                         std::size_t field,
                         std::uint32_t expectedClass,
                         std::size_t stride,
                         Array& rows) noexcept {
    return find_array_at(bytes, field, rows) && rows.elementClass == expectedClass
           && rows.dataOffset <= bytes.size()
           && rows.count <= (bytes.size() - rows.dataOffset) / stride;
}

/** The fixed fields and member rows of one quest-set block. */
struct QuestSet {
    std::uint8_t mode{};
    std::uint16_t valueSlot{};
    Array members{};
};

/**
 * Reads a quest set from its owner without judging its mode or value slot.
 * @param parent Set-owner bytes.
 * @param itemCount Exclusive bound for item-table indices; no set has more members.
 * @param set Receives the set; use only on success.
 * @return False when the block or its member array is missing or malformed.
 */
[[nodiscard]] bool
read_quest_set(std::span<const std::byte> parent, std::size_t itemCount, QuestSet& set) noexcept {
    std::size_t at = 0;
    return parent.size() >= kMinimumQuestDefinitionSize
           && block(parent, kItemQuestSetBlockOffset, kQuestSetBlockClass, at)
           && read(parent, at + kQuestSetModeOffset, set.mode)
           && read(parent, at + kQuestSetValueSlotOffset, set.valueSlot)
           && array(parent,
                    at + kQuestSetMemberDescriptor,
                    kQuestSetMemberClass,
                    kQuestSetMemberStride,
                    set.members)
           && set.members.count <= itemCount;
}

/**
 * Reads one member row.
 * @return False unless the reserved field is zero and the item is in the item table.
 */
[[nodiscard]] bool read_quest_member(std::span<const std::byte> parent,
                                     const Array& members,
                                     std::size_t index,
                                     std::size_t itemCount,
                                     std::int32_t& value,
                                     std::uint16_t& item) noexcept {
    const auto at = members.dataOffset + index * kQuestSetMemberStride;
    std::uint16_t reserved = 0;
    return read(parent, at + kQuestSetMemberValueOffset, value)
           && read(parent, at + kQuestSetMemberItemOffset, item)
           && read(parent, at + kQuestSetMemberReservedOffset, reserved) && reserved == 0
           && item < itemCount;
}

} // namespace

/**
 * A slot must match once across all maps, including maps with no supported save bank.
 * @param valueMap Blob containing all four unlock value maps.
 * @param slot Value slot to map.
 * @param row Receives the saved bank row; use only on success.
 * @param scope Receives the bank that row belongs to; use only on success.
 * @return False when the slot matches no row, several rows, or a map with no saved bank.
 */
bool map_value_slot(std::span<const std::byte> valueMap,
                    std::uint16_t slot,
                    std::uint16_t& row,
                    Quest::Scope& scope) noexcept {
    std::size_t matches = 0;
    for (const std::size_t descriptor : {kAccountValueMapDescriptor,
                                         kCharacterValueMapDescriptor,
                                         kThirdValueMapDescriptor,
                                         kFourthValueMapDescriptor}) {
        Array rows{};
        if (!find_optional_array_at(valueMap, descriptor, rows) || rows.dataOffset > valueMap.size()
            || rows.count > (valueMap.size() - rows.dataOffset) / kUnlockMapRowStride) {
            return false;
        }
        for (std::size_t i = 0; i < rows.count; ++i) {
            std::int16_t mappedSlot = -1;
            std::uint16_t reserved = 0;
            if (!read(valueMap,
                      rows.dataOffset + i * kUnlockMapRowStride + kUnlockMapDestinationSlotOffset,
                      mappedSlot)
                || !read(valueMap,
                         rows.dataOffset + i * kUnlockMapRowStride + kValueMapReservedOffset,
                         reserved)) {
                return false;
            }
            if (mappedSlot < 0 || static_cast<std::uint16_t>(mappedSlot) != slot) {
                continue;
            }
            if (reserved != 0 || ++matches != 1 || i >= kUnavailableValueMapRow
                || (descriptor != kAccountValueMapDescriptor
                    && descriptor != kCharacterValueMapDescriptor)) {
                return false;
            }
            row = static_cast<std::uint16_t>(i);
            scope = descriptor == kAccountValueMapDescriptor ? Quest::Scope::account
                                                             : Quest::Scope::character;
        }
    }
    return matches == 1;
}

/**
 * Only an objective-bearing pursuit can name a quest-set owner.
 * @param definition Item definition bytes, including its nested blocks.
 * @return The set owner's item-table index, or kUnavailableQuestParent on rejection.
 */
std::uint16_t quest_parent(std::span<const std::byte> definition) noexcept {
    std::uint8_t bucket = 0;
    std::size_t objective = 0;
    std::uint16_t parent = kUnavailableQuestParent;
    Array objectives{};
    if (definition.size() < kMinimumQuestDefinitionSize
        || !read(definition, kBucketIdOffset, bucket)
        || bucket != state::build_data::items::kPursuitBucketId
        || !block(definition, kItemObjectiveBlockOffset, kItemObjectiveBlockClass, objective)
        || !array(definition,
                  objective + kObjectiveArrayDescriptor,
                  kObjectiveReferenceArrayClass,
                  kObjectiveReferenceStride,
                  objectives)
        || !read(definition, objective + kObjectiveParentItemOffset, parent)) {
        return kUnavailableQuestParent;
    }
    return parent;
}

/**
 * Only a unique first member with one supported save-bank mapping may start a quest.
 * @param definition Pursuit item being acquired.
 * @param itemIndex Pursuit's item-table index.
 * @param parent Set-owner bytes selected by quest_parent; may be definition itself.
 * @param itemCount Exclusive bound for item-table indices.
 * @param valueMap Blob containing all four unlock value maps.
 * @return The first-step value and bank row, or an empty plan for unsupported content.
 */
Quest read_quest_initialization(std::span<const std::byte> definition,
                                std::uint16_t itemIndex,
                                std::span<const std::byte> parent,
                                std::size_t itemCount,
                                std::span<const std::byte> valueMap) noexcept {
    std::size_t unlock = 0;
    QuestSet set{};
    Array flags{};
    const auto parentIndex = quest_parent(definition);
    if (itemIndex >= itemCount || parentIndex >= itemCount
        || !read_quest_set(parent, itemCount, set) || set.mode != kSupportedQuestSetMode
        || set.valueSlot >= kUnlockSlotLimit) {
        return {};
    }
    std::int64_t unlockRelative = 0;
    if (!read(definition, kItemUnlockBlockOffset, unlockRelative)
        || (unlockRelative != 0
            && (!block(definition, kItemUnlockBlockOffset, kItemUnlockBlockClass, unlock)
                || !find_optional_array_at(definition, unlock + kItemPresenceFlagDescriptor, flags)
                || (flags.count != 0
                    && !array(definition,
                              unlock + kItemPresenceFlagDescriptor,
                              kItemPresenceFlagClass,
                              kItemPresenceFlagStride,
                              flags))))) {
        return {};
    }
    for (std::size_t i = 0; i < flags.count; ++i) {
        std::uint16_t flag = 0;
        if (!read(definition, flags.dataOffset + i * kItemPresenceFlagStride, flag)
            || flag >= kUnlockSlotLimit) {
            return {};
        }
    }
    // Without presence flags, require a separate objective-free root and a character value.
    const bool separateRoot = flags.count == 0;
    if (separateRoot) {
        std::uint8_t parentBucket = 0;
        std::int64_t parentObjective = 0;
        if (parentIndex == itemIndex || !read(parent, kBucketIdOffset, parentBucket)
            || parentBucket != kSeparateQuestRootBucketId
            || !read(parent, kItemObjectiveBlockOffset, parentObjective) || parentObjective != 0) {
            return {};
        }
    }
    Quest quest{};
    std::size_t matches = 0;
    for (std::size_t i = 0; i < set.members.count; ++i) {
        std::int32_t value = 0;
        std::uint16_t member = 0;
        if (!read_quest_member(parent, set.members, i, itemCount, value, member)) {
            return {};
        }
        if (member == itemIndex) {
            if (i != 0) {
                return {};
            }
            ++matches;
            quest.value = value;
        } else if (i != 0 && matches != 0 && value == quest.value) {
            return {}; // The first value must identify only one step.
        }
    }
    if (matches != 1 || !map_value_slot(valueMap, set.valueSlot, quest.row, quest.scope)) {
        return {};
    }
    return (!separateRoot || quest.scope == Quest::Scope::character)
                   && state::build_data::items::valid(quest)
               ? quest
               : Quest{};
}

/**
 * Members must name distinct items and step values, since the saved value selects the step.
 * @param definition Pursuit item.
 * @param itemIndex Pursuit's item-table index.
 * @param parent Set-owner bytes selected by quest_parent; may be definition itself.
 * @param itemCount Exclusive bound for item-table indices.
 * @param valueMap Blob containing all four unlock value maps.
 * @return The item's step and the member after it; no membership when no set names the item or
 *         the item is its set's only member; unsupported for any other set.
 */
QuestStep read_quest_step(std::span<const std::byte> definition,
                          std::uint16_t itemIndex,
                          std::span<const std::byte> parent,
                          std::size_t itemCount,
                          std::span<const std::byte> valueMap) noexcept {
    using Membership = state::build_data::pursuits::QuestMembership;
    const auto parentIndex = quest_parent(definition);
    if (parentIndex == kUnavailableQuestParent) {
        return {};
    }
    const QuestStep unsupported{Membership::unsupported};
    QuestSet set{};
    if (itemIndex >= itemCount || parentIndex >= itemCount
        || !read_quest_set(parent, itemCount, set)) {
        return unsupported;
    }
    QuestStep step{Membership::step};
    std::size_t position = set.members.count;
    for (std::size_t i = 0; i < set.members.count; ++i) {
        std::int32_t value = 0;
        std::uint16_t member = 0;
        if (!read_quest_member(parent, set.members, i, itemCount, value, member)) {
            return unsupported;
        }
        for (std::size_t prior = 0; prior < i; ++prior) {
            std::int32_t priorValue = 0;
            std::uint16_t priorMember = 0;
            if (!read_quest_member(parent, set.members, prior, itemCount, priorValue, priorMember)
                || priorValue == value || priorMember == member) {
                return unsupported;
            }
        }
        if (member == itemIndex) {
            position = i;
            step.value = value;
        } else if (position + 1 == i) {
            step.successorIndex = member;
            step.successorValue = value;
        }
    }
    if (position == set.members.count) {
        return unsupported;
    }
    if (set.members.count == 1) {
        return {};
    }
    // A zero step would read as a quest never started.
    if (set.mode != kSupportedQuestSetMode || set.valueSlot >= kUnlockSlotLimit
        || !map_value_slot(valueMap, set.valueSlot, step.row, step.scope)
        || !state::build_data::items::valid_bank_row(step.scope, step.row)
        || step.value == state::build_data::items::kUnsetQuestValue
        || (step.successorIndex != state::build_data::pursuits::kAbsent
            && step.successorValue == state::build_data::items::kUnsetQuestValue)) {
        return unsupported;
    }
    return step;
}

} // namespace sunrise::middleware::content::packages::tables::items
