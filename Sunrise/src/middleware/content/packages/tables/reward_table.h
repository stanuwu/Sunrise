#pragma once

#include <cstddef>
#include <cstdint>

#include "../reader/container_framing.h"
#include "../schema/generated/tag_classes_808029.h"
#include "../schema/generated/tag_classes_808030.h"
#include "../schema/generated/tag_classes_808074.h"
#include "../schema/generated/tag_classes_808075.h"
#include "../schema/generated/tag_classes_808077.h"
#include "../schema/generated/tag_classes_80807b.h"
#include "../schema/generated/tag_classes_80807c.h"
#include "../schema/generated/tag_classes_80807d.h"
#include "definition_index_table.h"

namespace sunrise::middleware::content::packages::tables::rewards {

namespace generated = schema::generated;

/** A root slot holding all ones names a table the build omits on purpose. */
inline constexpr std::uint32_t kAbsentTableTag = 0xFFFFFFFFU;

/** Investment-root slots of the reward tables and the unlock tables their conditions bind. */
struct RootSlots {
    /** Class table; each class row names the default finisher whose gate is the class flag. */
    static constexpr std::size_t kClasses =
        root_slot(offsetof(generated::InvestmentRoot, slot12ClassTable));
    /** Supplemental reward table an entry's supplemental index names. */
    static constexpr std::size_t kSupplementalRewards =
        root_slot(offsetof(generated::InvestmentRoot, slot84SupplementalRewards));
    /** Reward pool table every wrapper and nested entry indexes. */
    static constexpr std::size_t kPools =
        root_slot(offsetof(generated::InvestmentRoot, slot88RewardPools));
    /** Shared expression pool that the expression opcode indexes by row. */
    static constexpr std::size_t kExpressionPool =
        root_slot(offsetof(generated::InvestmentRoot, slot109RewardExpressions));
    /** Unlock flag slot table, one row per flag slot an instruction can read. */
    static constexpr std::size_t kFlagSlots = kUnlockFlagSlotTableSlot;
    /** Unlock value slot table, one row per value slot an instruction can read. */
    static constexpr std::size_t kValueSlots =
        root_slot(offsetof(generated::InvestmentRoot, slot114UnlockValueTable));
};

/** Reward pool table and its rows: definition hash, then the entry array. */
struct PoolRow {
    /** Class of the table the pool slot names. */
    static constexpr std::uint32_t kTableClass = generated::RewardPoolTable::kClass;
    /** Element class of the pool row array. */
    static constexpr std::uint32_t kClass = generated::RewardPoolRow::kClass;
    static constexpr std::size_t kSize = sizeof(generated::RewardPoolRow);
    static constexpr std::size_t kHash = offsetof(generated::RewardPoolRow, unk0);
    static constexpr std::size_t kEntries = offsetof(generated::RewardPoolRow, items);
};

/** One pool entry: target, quantity, draw weight and three dependent arrays. */
struct EntryRow {
    /** Element class of a pool's entry array. */
    static constexpr std::uint32_t kClass = generated::RewardEntry::kClass;
    static constexpr std::size_t kSize = sizeof(generated::RewardEntry);
    /** Granted item index; all ones when the entry names none. */
    static constexpr std::size_t kItem = offsetof(generated::RewardEntry, item);
    static constexpr std::size_t kQuantity = offsetof(generated::RewardEntry, quantity);
    /** Nested pool index; all ones when the entry names none. */
    static constexpr std::size_t kPool = offsetof(generated::RewardEntry, pool);
    /** Supplemental reward row; all ones when the entry names none. */
    static constexpr std::size_t kSupplemental = offsetof(generated::RewardEntry, supplemental);
    /** Category tag a wrapper selection must match; the empty tag matches every selection. */
    static constexpr std::size_t kCategory = offsetof(generated::RewardEntry, category);
    /** Draw weight, a 32-bit float. */
    static constexpr std::size_t kWeight = offsetof(generated::RewardEntry, weight);
    /** Eligibility expression field. */
    static constexpr std::size_t kCondition =
        offsetof(generated::RewardEntry, visibilityData.unlockExpression);
    static constexpr std::size_t kModifiers = offsetof(generated::RewardEntry, alternateData);
    static constexpr std::size_t kSockets = offsetof(generated::RewardEntry, socketOverrides);
};

/** One weight modifier: a condition, then a value index or a literal weight. */
struct ModifierRow {
    /** Element class of an entry's modifier array. */
    static constexpr std::uint32_t kClass = generated::RewardModifier::kClass;
    static constexpr std::size_t kSize = sizeof(generated::RewardModifier);
    static constexpr std::size_t kCondition = offsetof(generated::RewardModifier, unlockExpression);
    /** Unlock value row that supplies the weight; all ones when the literal applies. */
    static constexpr std::size_t kValueIndex = offsetof(generated::RewardModifier, valueIndex);
    /** Literal replacement weight, a 32-bit float. */
    static constexpr std::size_t kValue = offsetof(generated::RewardModifier, value);
};

/** One socket override, shared by pool entries and progression rewards. */
struct SocketOverrideRow {
    /** Element class of a socket override array. */
    static constexpr std::uint32_t kClass = generated::RewardSocketOverride::kClass;
    static constexpr std::size_t kSize = sizeof(generated::RewardSocketOverride);
    static constexpr std::size_t kSocketType =
        offsetof(generated::RewardSocketOverride, socketType);
    static constexpr std::size_t kPlugItem = offsetof(generated::RewardSocketOverride, plug);
    static constexpr std::size_t kPlugSet = offsetof(generated::RewardSocketOverride, plugSet);
    static constexpr std::size_t kRollSet = offsetof(generated::RewardSocketOverride, rollSet);
    /** Plug choice; all ones names the fixed plug item. */
    static constexpr std::size_t kSelection = offsetof(generated::RewardSocketOverride, selection);
};

/** Item definition fields the reward pass reads. */
struct ItemFields {
    /** Self-relative pointer to the use-condition block; it gates the class finishers. */
    static constexpr std::size_t kUseCondition =
        offsetof(generated::ItemDefinition, finisherItemBlock);
    /** Self-relative pointer to the wrapper block; zero when the item is not a wrapper. */
    static constexpr std::size_t kWrapper = offsetof(generated::ItemDefinition, sackItemBlock);
    /** Unlock flag slot the item sets when acquired; all ones when it sets none. */
    static constexpr std::size_t kAcquiredFlag =
        offsetof(generated::ItemDefinition, acquiredUnlockFlagSlot);
};

/** Use-condition block an item's use-condition pointer names; it opens with an expression. */
struct UseCondition {
    /** Schema class stored before the block. */
    static constexpr std::uint32_t kClass = generated::UseCondition::kClass;
};

/** Wrapper block: pool index, category selections, then the wrapper flags. */
struct WrapperBlock {
    /** Schema class stored before the block. */
    static constexpr std::uint32_t kClass = generated::RewardWrapper::kClass;
    static constexpr std::size_t kPool = offsetof(generated::RewardWrapper, pool);
    static constexpr std::size_t kSelections =
        offsetof(generated::RewardWrapper, rewardProperties.categoryList);
    /** Bit 0 opens the wrapper on acquisition. */
    static constexpr std::size_t kFlags = offsetof(generated::RewardWrapper, flags);
};

/** One wrapper selection: a category tag and how many draws it takes. */
struct SelectionRow {
    /** Element class of a wrapper's selection array. */
    static constexpr std::uint32_t kClass = generated::RewardSelection::kClass;
    static constexpr std::size_t kSize = sizeof(generated::RewardSelection);
    static constexpr std::size_t kCategory = offsetof(generated::RewardSelection, unk0);
    static constexpr std::size_t kCount = offsetof(generated::RewardSelection, count);
};

/** Unlock slot tables; each row opens with the unlock hash an external slot keeps. */
struct SlotRow {
    /** Classes of the flag slot table and of its row array. */
    static constexpr std::uint32_t kFlagTableClass = generated::UnlockFlagTable::kClass;
    static constexpr std::uint32_t kFlagClass = generated::UnlockFlagBinding::kClass;
    /** Classes of the value slot table and of its row array. */
    static constexpr std::uint32_t kValueTableClass = generated::UnlockValueTable::kClass;
    static constexpr std::uint32_t kValueClass = generated::UnlockValueBinding::kClass;
    static constexpr std::size_t kSize = kUnlockSlotRowStride;
    static constexpr std::size_t kHash = offsetof(generated::UnlockFlagBinding, unlockHash);
    /** Value rows only, unnamed in gap4: the kind of value the slot reads, then its flags. */
    static constexpr std::size_t kValueKind = offsetof(generated::UnlockValueBinding, gap4);
    static constexpr std::size_t kValueFlags = kValueKind + 1;
    /**
     * With an item in context, the client reads a slot of this kind from the item's instance
     * record instead of the bank, provided its flags are none, kItemValueIgnoredFlag alone or
     * kItemValueMaximumFlag alone.
     */
    static constexpr std::uint8_t kItemValueKind = 4;
    static constexpr std::uint8_t kItemValueIgnoredFlag = 0x04;
    /**
     * The client reads the slot's largest value across the character's held instances that carry
     * it. The shipped build sets it on 386 item-value slots, which 317 pursuits read.
     */
    static constexpr std::uint8_t kItemValueMaximumFlag = 0x02;
};
// Flag and value slot rows share the hash offset.
static_assert(SlotRow::kHash == offsetof(generated::UnlockValueBinding, unlockHash));
static_assert(SlotRow::kValueKind == 4);

/** Shared expression pool and its rows; each row holds one expression field. */
struct ExpressionPoolRow {
    /** Classes of the pool table and of its row array. */
    static constexpr std::uint32_t kTableClass = generated::RewardExpressionTable::kClass;
    static constexpr std::uint32_t kClass = generated::RewardExpressionRow::kClass;
    static constexpr std::size_t kSize = sizeof(generated::RewardExpressionRow);
    static constexpr std::size_t kExpression =
        offsetof(generated::RewardExpressionRow, unlockExpression);
};

/** One progression-reward condition: a bare expression field. */
struct ConditionRow {
    /** Element class of a progression reward's condition array. */
    static constexpr std::uint32_t kClass = generated::RewardCondition::kClass;
    static constexpr std::size_t kSize = kUnlockExpressionFieldSize;
};

/** Class table and its rows; one row per character class. */
struct ClassRow {
    /** Classes of the class table and of its row array. */
    static constexpr std::uint32_t kTableClass = generated::CharacterClassTable::kClass;
    static constexpr std::uint32_t kClass = generated::CharacterClassRow::kClass;
    static constexpr std::size_t kSize = sizeof(generated::CharacterClassRow);
    /** Item index of the class's default finisher. */
    static constexpr std::size_t kDefaultFinisher =
        offsetof(generated::CharacterClassRow, defaultFinisherItem);
};

} // namespace sunrise::middleware::content::packages::tables::rewards
