#include "quest_initialization_reader.h"

#include <algorithm>
#include <cstddef>
#include <limits>

#include "../reader/layout.h"
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

/** Policy: shorter item blobs stay outside the supported quest layout. */
constexpr std::size_t kMinimumQuestDefinitionSize = sizeof(generated::ItemDefinition);
/** A serialized block's 32-bit class ID sits immediately before its payload. */
constexpr std::size_t kBlockClassPrefixSize = reader::layout::kClassWordSize;
/** Policy: every quest block keeps at least the smallest quest block's fixed part. */
constexpr std::size_t kMinimumQuestBlockSize = (std::min)({sizeof(generated::ItemObjectiveBlock),
                                                           sizeof(generated::QuestSetBlock),
                                                           sizeof(generated::ItemUnlockBlock)});

/** Signed 64-bit offset to the objective block, relative to the field. */
constexpr std::size_t kItemObjectiveBlockOffset =
    offsetof(generated::ItemDefinition, objectiveItemBlock);
/** This block holds objective indices and the item index that owns the quest set. */
constexpr std::uint32_t kItemObjectiveBlockClass = generated::ItemObjectiveBlock::kClass;
/** A 16-bit item-table index, not a definition hash. */
constexpr std::size_t kObjectiveParentItemOffset =
    offsetof(generated::ItemObjectiveBlock, parentItemIndex);
/** Objective array entries are 16-bit table indices. */
constexpr std::size_t kObjectiveArrayDescriptor =
    offsetof(generated::ItemObjectiveBlock, objectives);
constexpr std::size_t kObjectiveReferenceStride = sizeof(generated::ObjectiveReferenceArray);

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

/** Unlock block offset, relative to the field; zero means absent. */
constexpr std::size_t kItemUnlockBlockOffset = offsetof(generated::ItemDefinition, unlockItemBlock);
/** This block's first array names the flags supplied by item presence. */
constexpr std::uint32_t kItemUnlockBlockClass = generated::ItemUnlockBlock::kClass;
/** Presence-flag entries hold unlock slot indices, not saved bank rows. */
constexpr std::uint32_t kItemPresenceFlagClass = generated::ItemPresenceFlag::kClass;
constexpr std::size_t kItemPresenceFlagDescriptor =
    offsetof(generated::ItemUnlockBlock, presenceFlags);
/** Each presence-flag entry occupies one 16-bit slot. */
constexpr std::size_t kItemPresenceFlagStride = sizeof(generated::ItemPresenceFlag);
/** Authored value/flag slots must fit the nonnegative range of a signed 16-bit mapping. */
constexpr std::uint16_t kUnlockSlotLimit = 0x8000U;
/** Policy: pursuits without presence flags need a bucket-37 root with no objective block. */
constexpr std::uint8_t kSeparateQuestRootBucketId = 37;

/** The third map has no supported save bank; a matching slot makes initialization unsafe. */
constexpr std::size_t kThirdValueMapDescriptor = offsetof(generated::C80807c80, h201ada27);
/** The fourth map is also checked for duplicate slots but has no supported save bank. */
constexpr std::size_t kFourthValueMapDescriptor = offsetof(generated::C80807c80, hD4a626c1);
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
    std::int64_t relative = 0;
    if (!read(bytes, field, relative) || relative == 0
        || relative
               > (std::numeric_limits<std::int64_t>::max)() - static_cast<std::int64_t>(field)) {
        return false;
    }
    const auto target = relative + static_cast<std::int64_t>(field);
    if (target < static_cast<std::int64_t>(kBlockClassPrefixSize)
        || static_cast<std::uint64_t>(target) > bytes.size()
        || bytes.size() - static_cast<std::size_t>(target) < kMinimumQuestBlockSize) {
        return false;
    }
    offset = static_cast<std::size_t>(target);
    std::uint32_t actualClass = 0;
    return reader::layout::class_word_before(bytes, offset, actualClass)
           && actualClass == expectedClass;
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

} // namespace

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
    std::size_t set = 0;
    std::size_t unlock = 0;
    std::uint8_t mode = 0;
    std::uint16_t slot = 0;
    Array members{}, flags{};
    const auto parentIndex = quest_parent(definition);
    if (itemIndex >= itemCount || parentIndex >= itemCount
        || parent.size() < kMinimumQuestDefinitionSize
        || !block(parent, kItemQuestSetBlockOffset, kQuestSetBlockClass, set)
        || !read(parent, set + kQuestSetModeOffset, mode) || mode != kSupportedQuestSetMode
        || !read(parent, set + kQuestSetValueSlotOffset, slot) || slot >= kUnlockSlotLimit
        || !array(parent,
                  set + kQuestSetMemberDescriptor,
                  kQuestSetMemberClass,
                  kQuestSetMemberStride,
                  members)
        || members.count > itemCount) {
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
    for (std::size_t i = 0; i < members.count; ++i) {
        const auto at = members.dataOffset + i * kQuestSetMemberStride;
        std::int32_t value = 0;
        std::uint16_t member = 0, reserved = 0;
        if (!read(parent, at + kQuestSetMemberValueOffset, value)
            || !read(parent, at + kQuestSetMemberItemOffset, member)
            || !read(parent, at + kQuestSetMemberReservedOffset, reserved) || reserved != 0
            || member >= itemCount) {
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
    if (matches != 1) {
        return {};
    }

    // A slot must match once across all maps, including maps with no supported save bank.
    matches = 0;
    for (const std::size_t descriptor : {kAccountValueMapDescriptor,
                                         kCharacterValueMapDescriptor,
                                         kThirdValueMapDescriptor,
                                         kFourthValueMapDescriptor}) {
        Array rows{};
        if (!find_optional_array_at(valueMap, descriptor, rows) || rows.dataOffset > valueMap.size()
            || rows.count > (valueMap.size() - rows.dataOffset) / kUnlockMapRowStride) {
            return {};
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
                return {};
            }
            if (mappedSlot < 0 || static_cast<std::uint16_t>(mappedSlot) != slot) {
                continue;
            }
            if (reserved != 0 || ++matches != 1 || i >= kUnavailableValueMapRow
                || (descriptor != kAccountValueMapDescriptor
                    && descriptor != kCharacterValueMapDescriptor)) {
                return {};
            }
            quest.row = static_cast<std::uint16_t>(i);
            quest.scope = descriptor == kAccountValueMapDescriptor ? Quest::Scope::account
                                                                   : Quest::Scope::character;
        }
    }
    return matches == 1 && (!separateRoot || quest.scope == Quest::Scope::character)
                   && state::build_data::items::valid(quest)
               ? quest
               : Quest{};
}

} // namespace sunrise::middleware::content::packages::tables::items
