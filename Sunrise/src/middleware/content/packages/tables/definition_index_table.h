#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../reader/layout.h"
#include "../schema/generated/tag_classes_80802c.h"
#include "../schema/generated/tag_classes_80802f.h"
#include "../schema/generated/tag_classes_808030.h"
#include "../schema/generated/tag_classes_808034.h"
#include "../schema/generated/tag_classes_808059.h"
#include "../schema/generated/tag_classes_80805a.h"
#include "../schema/generated/tag_classes_80805b.h"
#include "../schema/generated/tag_classes_80805c.h"
#include "../schema/generated/tag_classes_80805d.h"
#include "../schema/generated/tag_classes_808074.h"
#include "../schema/generated/tag_classes_808077.h"
#include "../schema/generated/tag_classes_808078.h"
#include "../schema/generated/tag_classes_80807a.h"
#include "../schema/generated/tag_classes_80807b.h"
#include "../schema/generated/tag_classes_80807c.h"
#include "../schema/generated/tag_classes_80807d.h"
#include "../schema/generated/tag_classes_808087.h"
#include "../schema/generated/tag_classes_80809f.h"

namespace sunrise::middleware::content::packages::tables {

/** Definition tags sit in [kTagLowerBound, kTagUpperBound). */
inline constexpr std::uint32_t kTagLowerBound = reader::layout::kTagBase;
/** Values above this are not definition tags. */
inline constexpr std::uint32_t kTagUpperBound = 0x82000000;
/** A tag holds its entry index in the low bits and its package id above them. */
inline constexpr std::uint32_t kTagPackageShift = reader::layout::kTagEntryBits;
/** The package id no tag names, reported for a value outside the tag range. */
inline constexpr std::uint16_t kAbsentPackageId = 0xFFFFU;

/**
 * Reads the package id one tag names.
 * It is the same number the package file carries in its name.
 * @param tag Definition tag.
 * @return The package id, or `kAbsentPackageId` when the value is not a tag.
 */
[[nodiscard]] constexpr std::uint16_t package_of(std::uint32_t tag) noexcept {
    if (tag < kTagLowerBound || tag >= kTagUpperBound) {
        return kAbsentPackageId;
    }
    return static_cast<std::uint16_t>((tag - kTagLowerBound) >> kTagPackageShift);
}

/** Root slots are 16-byte rows after the prefix; each row opens with its tag word. */
inline constexpr std::size_t kRootSlotTableOffset =
    offsetof(schema::generated::InvestmentRoot, unk8);
inline constexpr std::size_t kRootSlotStride =
    offsetof(schema::generated::InvestmentRoot, slot12ClassTable)
    - offsetof(schema::generated::InvestmentRoot, slot11InvestmentConstants);

/** @return The ordinal of the root slot whose tag word sits at `offset`. */
consteval std::size_t root_slot(std::size_t offset) {
    if (offset < kRootSlotTableOffset || (offset - kRootSlotTableOffset) % kRootSlotStride != 0) {
        throw "offset is not a root slot tag word";
    }
    return (offset - kRootSlotTableOffset) / kRootSlotStride;
}

/** The globals container's children are 16-byte rows; the view lists them as flat members. */
inline constexpr std::size_t kGlobalsChildTableOffset =
    offsetof(schema::generated::InvestmentGlobals, investmentRoot);
inline constexpr std::size_t kGlobalsChildStride =
    offsetof(schema::generated::InvestmentGlobals, unk20)
    - offsetof(schema::generated::InvestmentGlobals, investmentRoot);

/** @return The ordinal of the globals child whose tag word sits at `offset`. */
consteval std::size_t globals_child(std::size_t offset) {
    if (offset < kGlobalsChildTableOffset
        || (offset - kGlobalsChildTableOffset) % kGlobalsChildStride != 0) {
        throw "offset is not a globals child tag word";
    }
    return (offset - kGlobalsChildTableOffset) / kGlobalsChildStride;
}

/** Element class of the item index table inside the investment container. */
inline constexpr std::uint32_t kItemIndexTableClass = schema::generated::ItemIndexRow::kClass;
/** Serialized item definition class, distinct from the item index table class. */
inline constexpr std::uint32_t kItemDefinitionClass = schema::generated::ItemDefinition::kClass;
/** Investment root slot of the records and lore table. */
inline constexpr std::size_t kRecordTableSlot =
    root_slot(offsetof(schema::generated::InvestmentRoot, slot72Records));
/** One record row, wider than any field this pass reads. */
inline constexpr std::size_t kRecordRowStride = sizeof(schema::generated::RecordRow);
/** Unlock slot of the record's completion flag, or a non-positive value when it has none. */
inline constexpr std::size_t kRecordCompletionFlagOffset =
    offsetof(schema::generated::RecordRow, completionFlagSlot);
/** Authored record definition hash inside one record row. */
inline constexpr std::size_t kRecordHashOffset = offsetof(schema::generated::RecordRow, recordHash);
// A node row carries its hash at the record hash offset, so one constant reads both.
static_assert(kRecordHashOffset == offsetof(schema::generated::PresentationNodeRow, nodeHash));
/** Lore row a record names, or 0xFFFF when it names none. */
inline constexpr std::size_t kLoreRowOffset = offsetof(schema::generated::RecordRow, loreRow);
// TODO: read the lore table so incident grants stop authoring their lore ordinal ranges.
/** Investment root slot of the lore table. */
inline constexpr std::size_t kLoreTableSlot =
    root_slot(offsetof(schema::generated::InvestmentRoot, slot52Lore));
/** One lore row, and the definition hash inside it. */
inline constexpr std::size_t kLoreRowStride = sizeof(schema::generated::LoreRow);
inline constexpr std::size_t kLoreHashOffset = offsetof(schema::generated::LoreRow, loreHash);
/** Points the record is worth. Zero for lore and for the interval records that score per step. */
inline constexpr std::size_t kRecordScoreOffset = offsetof(schema::generated::RecordRow, score);
/** A record names its category's value slot here. The record's own bar reads the next slot up. */
inline constexpr std::size_t kRecordCategoryExpressionField =
    offsetof(schema::generated::RecordRow, hE5440b42);
/** A record's other expression field. A presentation gate sits in either of the two. */
inline constexpr std::size_t kRecordAlternateExpressionField =
    offsetof(schema::generated::RecordRow, h38d50966);
/** Objective rows a record names, two bytes each. */
inline constexpr std::size_t kRecordObjectiveField =
    offsetof(schema::generated::RecordRow, objectives);
inline constexpr std::uint32_t kRecordObjectiveRowClass =
    schema::generated::RecordObjectiveRef::kClass;
inline constexpr std::size_t kRecordObjectiveStride = sizeof(schema::generated::RecordObjectiveRef);
/** Interval rows a record names: objective row, score, then the item index the step grants. */
inline constexpr std::size_t kRecordIntervalField =
    offsetof(schema::generated::RecordRow, h3f2b8a2a.intervalObjectives);
inline constexpr std::uint32_t kRecordIntervalRowClass =
    schema::generated::RecordIntervalRow::kClass;
inline constexpr std::size_t kRecordIntervalStride = sizeof(schema::generated::RecordIntervalRow);
/** The objective row is two bytes; two pad bytes follow it. */
inline constexpr std::size_t kRecordIntervalObjectiveRowOffset =
    offsetof(schema::generated::RecordIntervalRow, objectiveRow);
inline constexpr std::size_t kRecordIntervalScoreOffset =
    offsetof(schema::generated::RecordIntervalRow, score);
inline constexpr std::size_t kRecordIntervalItemIndexOffset =
    offsetof(schema::generated::RecordIntervalRow, itemIndex);
/** Unlock value slot counting a record's redeemed intervals, or 0xFFFF when it has none. */
inline constexpr std::size_t kRecordRedeemedCountSlotOffset =
    offsetof(schema::generated::RecordRow, h3f2b8a2a.redeemedCountSlot);
/** An objective holds the expression naming its progress source at this field. */
inline constexpr std::size_t kObjectiveSourceExpressionField =
    offsetof(schema::generated::ObjectiveRow, hC198a43a);
/** Installed tag of the record display table. Its rows sit in record order. */
inline constexpr std::uint32_t kRecordDisplayTableTag = 0x81613CFFU;
inline constexpr std::size_t kRecordDisplayRowStride = sizeof(schema::generated::RecordDisplayRow);
inline constexpr std::uint32_t kRecordDisplayRowClass = schema::generated::RecordDisplayRow::kClass;
/** Reward rows a record grants, held on its display row: item index then quantity. */
inline constexpr std::size_t kRecordRewardField =
    offsetof(schema::generated::RecordDisplayRow, rewards);
inline constexpr std::uint32_t kRecordRewardRowClass = schema::generated::RecordRewardRow::kClass;
inline constexpr std::size_t kRecordRewardStride = sizeof(schema::generated::RecordRewardRow);
/** The item index is two bytes; two pad bytes follow it. */
inline constexpr std::size_t kRecordRewardItemIndexOffset =
    offsetof(schema::generated::RecordRewardRow, itemIndex);
inline constexpr std::size_t kRecordRewardQuantityOffset =
    offsetof(schema::generated::RecordRewardRow, quantity);
/**
 * Nonzero when the record's completion grants a character-equippable title.
 * The flag is one byte; seven pad bytes follow it.
 */
inline constexpr std::size_t kRecordHasTitleOffset =
    offsetof(schema::generated::RecordRow, hasTitle);
/** Investment root slot of the four unlock value mapping tables. */
inline constexpr std::size_t kUnlockValueMapTableSlot =
    root_slot(offsetof(schema::generated::InvestmentRoot, slot113UnlockValueMaps));
/** Array descriptor of the account object's value mapping table. */
inline constexpr std::size_t kAccountValueMapDescriptor =
    offsetof(schema::generated::C80807c80, h826fd8c8);
/** One unlock expression instruction: an opcode then its operand. */
inline constexpr std::size_t kUnlockInstructionStride =
    sizeof(schema::generated::InvestmentExpressionRow);
/** The operand within one unlock expression instruction. */
inline constexpr std::size_t kUnlockInstructionOperandOffset =
    offsetof(schema::generated::InvestmentExpressionRow, operand);
/** An expression field is a 64-bit count then a 64-bit self-relative offset. */
inline constexpr std::size_t kUnlockExpressionFieldSize =
    sizeof(schema::generated::RewardCondition);
inline constexpr std::size_t kUnlockExpressionCountOffset =
    offsetof(schema::generated::RewardCondition, expressionSteps.count);
inline constexpr std::size_t kUnlockExpressionPointerOffset =
    offsetof(schema::generated::RewardCondition, expressionSteps.relative);
/** The opcode within one unlock expression instruction. */
inline constexpr std::size_t kUnlockInstructionOpcodeOffset =
    offsetof(schema::generated::InvestmentExpressionRow, opcode);
/** The evaluator's opcodes run to 28; anything wider means the field is not an expression. */
inline constexpr std::uint32_t kUnlockOpcodeCeiling = 28;
/** The opcode that reads a value slot. */
inline constexpr std::uint32_t kUnlockReadValueOpcode = 10;
/** The opcode that tests a flag. */
inline constexpr std::uint32_t kUnlockReadFlagOpcode = 1;
/** The unary opcodes: logical NOT, negation, hash and bitwise complement each pop one operand. */
inline constexpr std::uint32_t kUnlockNotOpcode = 2;
inline constexpr std::uint32_t kUnlockNegateOpcode = 22;
inline constexpr std::uint32_t kUnlockHashOpcode = 23;
inline constexpr std::uint32_t kUnlockBitNotOpcode = 28;
/** The opcode that joins two operands by logical AND. */
inline constexpr std::uint32_t kUnlockAndOpcode = 4;
/** The opcode that pushes a literal. */
inline constexpr std::uint32_t kUnlockLiteralOpcode = 11;
/** The opcode that expands a shared expression row. Its operand is the row index. */
inline constexpr std::uint32_t kUnlockExpressionOpcode = 12;
/** The opcode that tests greater than or equal. */
inline constexpr std::uint32_t kUnlockGreaterEqualOpcode = 14;
/** Stops a wild count being walked as an expression. One shipped book carries 59 instructions. */
inline constexpr std::int64_t kNodeExpressionCapacity = 128;
/** Investment root slot of the presentation node table. */
inline constexpr std::size_t kPresentationNodeTableSlot =
    root_slot(offsetof(schema::generated::InvestmentRoot, slot63PresentationNodes));
/** One node row. */
inline constexpr std::size_t kNodeRowStride = sizeof(schema::generated::PresentationNodeRow);
/** A node's expression sits at one of these two fields, never both. */
inline constexpr std::size_t kNodeExpressionFieldPrimary =
    offsetof(schema::generated::PresentationNodeRow, h38d50966);
inline constexpr std::size_t kNodeExpressionFieldAlternate =
    offsetof(schema::generated::PresentationNodeRow, hD61b5e99);
/** Records a node owns, four bytes each as a row then a gate. */
inline constexpr std::size_t kNodeChildRecordField =
    offsetof(schema::generated::PresentationNodeRow, children.records);
inline constexpr std::size_t kNodeChildRecordStride =
    sizeof(schema::generated::PresentationNodeRecordRef);
/**
 * Array descriptor of the profile flag mapping table, between the account (+8) and character (+40)
 * maps. Profile flags are account-wide but saved in their own bank, not the account flag bank.
 */
inline constexpr std::size_t kProfileFlagMapDescriptor =
    offsetof(schema::generated::C80807d36, hC93e5c6b);
/** Array descriptor of the character object's flag mapping table, sized to that bank. */
inline constexpr std::size_t kCharacterFlagMapDescriptor =
    offsetof(schema::generated::C80807d36, h1c76a86c);
/** Array descriptor of the character object's value mapping table, sized to that bank. */
inline constexpr std::size_t kCharacterValueMapDescriptor =
    offsetof(schema::generated::C80807c80, h1c76a86c);
/**
 * Array descriptor of the fourth value mapping table. Its rows are the values every character's
 * block in the account object holds; the shipped build maps 33.
 */
inline constexpr std::size_t kCharacterBlockValueMapDescriptor =
    offsetof(schema::generated::C80807c80, hD4a626c1);
/** Investment root slot of the unlock flag mapping tables. */
inline constexpr std::size_t kUnlockFlagMapTableSlot =
    root_slot(offsetof(schema::generated::InvestmentRoot, slot111UnlockFlagMaps));
/** Array descriptor of the account object's flag mapping table. */
inline constexpr std::size_t kAccountFlagMapDescriptor =
    offsetof(schema::generated::C80807d36, h826fd8c8);
/** One mapping row: a source and the slot it feeds. */
inline constexpr std::size_t kUnlockMapRowStride = sizeof(schema::generated::UnlockFlagMapRow);
/** The destination slot within one mapping row. */
inline constexpr std::size_t kUnlockMapDestinationSlotOffset =
    offsetof(schema::generated::UnlockFlagMapRow, destinationSlot);
// Flag and value mapping rows share one stride and one destination offset.
static_assert(kUnlockMapRowStride == sizeof(schema::generated::UnlockValueMapRow));
static_assert(kUnlockMapDestinationSlotOffset
              == offsetof(schema::generated::UnlockValueMapRow, destinationSlot));

/** The investment root holds the installed collectible definition table at this slot. */
inline constexpr std::size_t kCollectibleTableSlot =
    root_slot(offsetof(schema::generated::InvestmentRoot, slot19Collectibles));
/** Definition class recorded for the installed investment root tag. */
inline constexpr std::uint32_t kInvestmentRootClass = schema::generated::InvestmentRoot::kClass;
/** Definition class recorded for the installed collectible table tag. */
inline constexpr std::uint32_t kCollectibleTableClass = schema::generated::CollectibleTable::kClass;
/** Element class recorded by the collectible table's inline array header. */
inline constexpr std::uint32_t kCollectibleRowClass = schema::generated::CollectibleRow::kClass;
/** One collectible row in this installed build occupies 184 bytes. */
inline constexpr std::size_t kCollectibleRowStride = sizeof(schema::generated::CollectibleRow);
/** Authored DestinyCollectibleDefinition hash inside one collectible row. */
inline constexpr std::size_t kCollectibleHashOffset =
    offsetof(schema::generated::CollectibleRow, collectibleHash);
/** Native item-definition index granted by one collectible row. */
inline constexpr std::size_t kCollectibleItemIndexOffset =
    offsetof(schema::generated::CollectibleRow, itemIndex);
/** Native material-requirement-set ordinal carried by one collectible row. */
inline constexpr std::size_t kCollectibleMaterialRequirementIndexOffset =
    offsetof(schema::generated::CollectibleRow, materialRequirementSet);
/** The investment root holds the material-requirement-set table at this slot. */
inline constexpr std::size_t kMaterialRequirementTableSlot =
    root_slot(offsetof(schema::generated::InvestmentRoot, slot96MaterialRequirements));
/** Installed material-requirement-set table and row classes. */
inline constexpr std::uint32_t kMaterialRequirementTableClass =
    schema::generated::MaterialRequirementTable::kClass;
inline constexpr std::uint32_t kMaterialRequirementSetRowClass =
    schema::generated::MaterialRequirementSetRow::kClass;
inline constexpr std::uint32_t kMaterialRequirementRowClass =
    schema::generated::MaterialRequirementRow::kClass;
/** One set row is a hash followed by a self-relative pointer to an array descriptor. */
inline constexpr std::size_t kMaterialRequirementSetRowStride =
    sizeof(schema::generated::MaterialRequirementSetRow);
inline constexpr std::size_t kMaterialRequirementSetHashOffset =
    offsetof(schema::generated::MaterialRequirementSetRow, setHash);
inline constexpr std::size_t kMaterialRequirementSetArrayPointerOffset =
    offsetof(schema::generated::MaterialRequirementSetRow, requirements);
/** One requirement row is item index, quantity, two flags, then a native sentinel. */
inline constexpr std::size_t kMaterialRequirementRowStride =
    sizeof(schema::generated::MaterialRequirementRow);
/** The item index is two bytes; two pad bytes follow it. */
inline constexpr std::size_t kMaterialRequirementItemIndexOffset =
    offsetof(schema::generated::MaterialRequirementRow, itemIndex);
inline constexpr std::size_t kMaterialRequirementQuantityOffset =
    offsetof(schema::generated::MaterialRequirementRow, quantity);
inline constexpr std::size_t kMaterialRequirementDeleteOffset =
    offsetof(schema::generated::MaterialRequirementRow, deleteOnAction);
inline constexpr std::size_t kMaterialRequirementOmitOffset =
    offsetof(schema::generated::MaterialRequirementRow, omitFromRequirements);
inline constexpr std::size_t kMaterialRequirementSentinelOffset =
    offsetof(schema::generated::MaterialRequirementRow, nativeSentinel);
/** Insertion and enabled requirement-set ordinals, relative to the item's plug block. */
inline constexpr std::size_t kInsertionMaterialRequirementSetIndexOffset =
    offsetof(schema::generated::PlugBlock, insertionRequirements.insertionMaterialRequirementSet);
inline constexpr std::size_t kEnabledMaterialRequirementSetIndexOffset =
    offsetof(schema::generated::PlugBlock, hAd24b7e3.enabledMaterialRequirementSet);
inline constexpr std::uint16_t kUnavailableMaterialRequirementSetIndex = 0xFFFFU;
/** Element class of an item's ordinary socket array. */
inline constexpr std::uint32_t kOrdinarySocketClass =
    schema::generated::ItemOrdinarySocketEntry::kClass;
/** One ordinary socket entry is 80 bytes. */
inline constexpr std::size_t kSocketEntryStride =
    sizeof(schema::generated::ItemOrdinarySocketEntry);
/** An item definition holds its socket block self-relative at this offset. */
inline constexpr std::size_t kSocketBlockOffset =
    offsetof(schema::generated::ItemDefinition, socketsItemBlock);
/** An item definition holds its inventory bucket id at this offset. */
inline constexpr std::size_t kBucketIdOffset =
    offsetof(schema::generated::ItemDefinition, inventoryItemBlock.inventoryBucket);
/** An item definition holds its art block self-relative at this offset, zero when absent. */
inline constexpr std::size_t kArtBlockOffset =
    offsetof(schema::generated::ItemDefinition, translationItemBlock);
/** Element class of an item's art row array. */
inline constexpr std::uint32_t kArtRowClass = schema::generated::ItemArtRow::kClass;
/** Element class of an item's material override array. */
inline constexpr std::uint32_t kMaterialOverrideClass =
    schema::generated::ItemMaterialOverride::kClass;
/** Element class of an item's sandbox perk array. */
inline constexpr std::uint32_t kSandboxPerkClass = schema::generated::ItemSandboxPerk::kClass;
/** An item definition holds its stat block self-relative at this offset, zero when absent. */
inline constexpr std::size_t kStatBlockOffset =
    offsetof(schema::generated::ItemDefinition, statsItemBlock);
/** The investment root holds the constants blob at this slot. */
inline constexpr std::size_t kInvestmentConstantsSlot =
    root_slot(offsetof(schema::generated::InvestmentRoot, slot11InvestmentConstants));

/** The investment root holds the progression definition table at this slot. */
inline constexpr std::size_t kProgressionTableSlot =
    root_slot(offsetof(schema::generated::InvestmentRoot, slot68Progressions));
/** Element class of the progression definition table. */
inline constexpr std::uint32_t kProgressionTableClass = schema::generated::ProgressionRow::kClass;
/** One progression definition is 88 inline bytes. */
inline constexpr std::size_t kProgressionRowStride = sizeof(schema::generated::ProgressionRow);
/** A progression definition opens with its definition hash. */
inline constexpr std::size_t kProgressionHashOffset =
    offsetof(schema::generated::ProgressionRow, unk0);
/** A progression definition names the object array holding its record here, in four bytes. */
inline constexpr std::size_t kProgressionScopeOffset =
    offsetof(schema::generated::ProgressionRow, scope);
/** The byte after the scope selector flags progressions that repeat their final step. */
inline constexpr std::size_t kProgressionRepeatLastStepOffset =
    kProgressionScopeOffset + sizeof(std::uint8_t);
/** Rank steps a progression declares: the experience each rank costs, in rank order. */
inline constexpr std::size_t kProgressionStepField =
    offsetof(schema::generated::ProgressionRow, steps);
inline constexpr std::uint32_t kProgressionStepRowClass =
    schema::generated::ProgressionStep::kClass;
inline constexpr std::size_t kProgressionStepStride = sizeof(schema::generated::ProgressionStep);
/** Reward rows a progression grants, held on the progression definition. */
inline constexpr std::size_t kProgressionRewardField =
    offsetof(schema::generated::ProgressionRow, rewardItems);
inline constexpr std::uint32_t kProgressionRewardRowClass =
    schema::generated::ProgressionReward::kClass;
inline constexpr std::size_t kProgressionRewardStride =
    sizeof(schema::generated::ProgressionReward);
/** Reward row fields: the rank it needs, the item it grants, and how many. */
inline constexpr std::size_t kProgressionRewardRankOffset =
    offsetof(schema::generated::ProgressionReward, rank);
/** The item index is two bytes; two pad bytes follow it. */
inline constexpr std::size_t kProgressionRewardItemIndexOffset =
    offsetof(schema::generated::ProgressionReward, itemIndex);
inline constexpr std::size_t kProgressionRewardQuantityOffset =
    offsetof(schema::generated::ProgressionReward, quantity);
/**
 * Unlock flag slot a reward's claim sets. The slots run dense in reward order.
 * The slot is two bytes; two pad bytes follow it.
 */
inline constexpr std::size_t kProgressionRewardClaimSlotOffset =
    offsetof(schema::generated::ProgressionReward, claimFlagSlot);
/** Eligibility expressions and socket overrides carried by one progression reward. */
inline constexpr std::size_t kProgressionRewardConditionsOffset =
    offsetof(schema::generated::ProgressionReward, conditions);
inline constexpr std::size_t kProgressionRewardSocketsOffset =
    offsetof(schema::generated::ProgressionReward, socketOverrides);

/** Expression that makes one collectible acquired, which a purchase or a grant sets. */
inline constexpr std::size_t kCollectibleAcquiredExpressionField =
    offsetof(schema::generated::CollectibleRow, hC5bb1f46);
/** Investment root slot of the unlock flag slot table, which is keyed by slot. */
inline constexpr std::size_t kUnlockFlagSlotTableSlot =
    root_slot(offsetof(schema::generated::InvestmentRoot, slot112UnlockFlagSlots));
/** One flag slot row, and the bank row it feeds inside the object its kind names. */
inline constexpr std::size_t kUnlockSlotRowStride = sizeof(schema::generated::UnlockFlagBinding);
inline constexpr std::size_t kUnlockSlotBankIndexOffset =
    offsetof(schema::generated::UnlockFlagBinding, bankIndex);
// Flag and value slot rows share one stride.
static_assert(kUnlockSlotRowStride == sizeof(schema::generated::UnlockValueBinding));

/** Installed tag of the per-item strings index table. Its rows sit in item index order. */
inline constexpr std::uint32_t kItemStringsIndexTag = 0x81613CF1U;
inline constexpr std::uint32_t kItemStringsIndexRowClass =
    schema::generated::ItemStringsIndexRow::kClass;
/**
 * Item-type pair on one item's strings blob: the bank, then the name hash inside it.
 * The bank is two bytes, 0xFFFF for none; two pad bytes follow it.
 */
inline constexpr std::size_t kItemStringsTypePairOffset =
    offsetof(schema::generated::ItemDisplayStrings, displayProperties.typeBank);
inline constexpr std::size_t kItemStringsTypeHashOffset =
    offsetof(schema::generated::ItemDisplayStrings, displayProperties.typeHash);

/** One array found inside a definition blob. */
struct Array {
    std::uint64_t count{};
    std::size_t dataOffset{};
    std::uint32_t elementClass{};
};

/** One row of a definition index table. */
struct IndexRow {
    std::uint32_t definitionHash{};
    std::uint32_t targetTag{};
};

// TODO: visit_index_rows has no caller yet; the open runtime extraction sources need it.
/** Visitor called once per index-table row. */
using RowVisitor = bool (*)(void* context, std::uint32_t index, const IndexRow& row) noexcept;

/**
 * Reads the array descriptor at one known offset.
 * @param blob Whole definition bytes.
 * @param descriptorOffset Offset of the count and self-relative offset pair.
 * @param output Receives the array that was found.
 * @return True when the descriptor points at a typed-array header that repeats its count.
 */
[[nodiscard]] bool find_array_at(std::span<const std::byte> blob,
                                 std::size_t descriptorOffset,
                                 Array& output) noexcept;

/**
 * Reads the array descriptor at one known offset and accepts an array with no entries.
 * A zero count is authored either with no header at all or with a header repeating that count.
 * Use this where an empty list is ordinary data rather than an unreadable one.
 * @param blob Whole definition bytes.
 * @param descriptorOffset Offset of the count and self-relative offset pair.
 * @param output Receives the array, with count zero when the list is empty.
 * @return True when the descriptor is a valid array or a valid empty one.
 */
[[nodiscard]] bool find_optional_array_at(std::span<const std::byte> blob,
                                          std::size_t descriptorOffset,
                                          Array& output) noexcept;

/** Reads an optional typed array and checks every fixed-stride row lies in its blob. */
[[nodiscard]] bool read_array(std::span<const std::byte> blob,
                              std::size_t at,
                              std::uint32_t elementClass,
                              std::size_t stride,
                              Array& rows) noexcept;

/**
 * Finds the first array whose header names one element class.
 * @param blob Whole definition bytes.
 * @param elementClass Element class the header must carry.
 * @param output Cleared before the search and filled only on a match.
 * @return True when one descriptor points at a header of that class.
 */
[[nodiscard]] bool
find_array(std::span<const std::byte> blob, std::uint32_t elementClass, Array& output) noexcept;

/**
 * Reads every row of one index table in order and hands each to a visitor.
 * @param blob Whole definition bytes.
 * @param array Index-table array that was found.
 * @param visitor Called once per row. Returning false ends the walk.
 * @param context Opaque pointer handed back to the visitor.
 * @return True when every row read back and the visitor accepted all of them.
 */
[[nodiscard]] bool visit_index_rows(std::span<const std::byte> blob,
                                    const Array& array,
                                    RowVisitor visitor,
                                    void* context) noexcept;

/**
 * Walks the child tags of one container blob.
 * @param blob Whole container bytes.
 * @param index Child ordinal.
 * @param tag Receives the child tag.
 * @return True when the ordinal is inside the container.
 */
[[nodiscard]] bool
child_tag(std::span<const std::byte> blob, std::size_t index, std::uint32_t& tag) noexcept;

/**
 * Reads one investment-root slot.
 * @param blob Whole root bytes.
 * @param index Slot ordinal.
 * @param tag Receives the slot tag.
 * @return True when the slot is inside the root.
 */
[[nodiscard]] bool
slot_tag(std::span<const std::byte> blob, std::size_t index, std::uint32_t& tag) noexcept;

/** The investment root holds the item index table at this slot. */
inline constexpr std::size_t kItemTableSlot =
    root_slot(offsetof(schema::generated::InvestmentRoot, slot48ItemIndex));
/** The investment root holds the new-character table, one row of items per class, here. */
inline constexpr std::size_t kStarterSetTableSlot =
    root_slot(offsetof(schema::generated::InvestmentRoot, slot15StarterSets));
/** The investment root holds the shared reusable/randomized plug-set table at this slot. */
inline constexpr std::size_t kPlugSetTableSlot =
    root_slot(offsetof(schema::generated::InvestmentRoot, slot51PlugSets));
/** The investment root holds the items ordinary grants refuse at this slot. */
inline constexpr std::size_t kRestrictedGrantTableSlot =
    root_slot(offsetof(schema::generated::InvestmentRoot, unk3b8));
// Unnamed in the generated root; the shipped slot 59 tag is a C80807879 whose list names one item.
static_assert(kRestrictedGrantTableSlot == 59);
/** The investment root holds the actor class race table at this slot. */
inline constexpr std::size_t kEnemyRaceTableSlot =
    root_slot(offsetof(schema::generated::InvestmentRoot, unk1a8));
// Unnamed in the generated root; the shipped slot 26 tag is a C80807bbb table of 66 class rows.
static_assert(kEnemyRaceTableSlot == 26);
/** Element class of the actor class race rows. */
inline constexpr std::uint32_t kEnemyRaceRowClass = schema::generated::C80807bc0::kClass;
/** The race table holds its rows in this array. */
inline constexpr std::size_t kEnemyRaceArrayDescriptor =
    offsetof(schema::generated::C80807bbb, table);
/** A row holds the class name hash, then the race ordinal in a word the schema leaves unnamed. */
inline constexpr std::size_t kEnemyRaceClassOffset = offsetof(schema::generated::C80807bc0, unk0);
inline constexpr std::size_t kEnemyRaceOrdinalOffset = offsetof(schema::generated::C80807bc0, gap4);
inline constexpr std::size_t kEnemyRaceRowStride = sizeof(schema::generated::C80807bc0);
/** Class of the restricted-grant list tag, and element class of its rows. */
inline constexpr std::uint32_t kRestrictedGrantTableClass = schema::generated::C80807879::kClass;
inline constexpr std::uint32_t kRestrictedGrantElementClass = schema::generated::C8080787c::kClass;
/** The list tag holds its rows in this array, after its size word. */
inline constexpr std::size_t kRestrictedGrantArrayDescriptor =
    offsetof(schema::generated::C80807879, constants.trackedItems);
static_assert(offsetof(schema::generated::C80807879, constants) == reader::layout::kSizeWordSize);
/** Each row holds the item-table index first; nothing the client reads follows it. */
inline constexpr std::size_t kRestrictedGrantStride = sizeof(schema::generated::C8080787c);
inline constexpr std::size_t kRestrictedGrantItemIndexOffset =
    offsetof(schema::generated::C8080787c, gap0);
/** The investment root holds the dense ordinary socket-type table at this slot. */
inline constexpr std::size_t kSocketTypeTableSlot =
    root_slot(offsetof(schema::generated::InvestmentRoot, slot94SocketTypes));
/** Element class and row size of the installed ordinary socket-type table. */
inline constexpr std::uint32_t kSocketTypeTableClass = schema::generated::SocketTypeRow::kClass;
inline constexpr std::size_t kSocketTypeRowStride = sizeof(schema::generated::SocketTypeRow);
/** One socket type points to its acquired-state rule array from this row offset. */
inline constexpr std::size_t kSocketTypeAcquisitionDescriptor =
    offsetof(schema::generated::SocketTypeRow, h0db6e645);
/**
 * One socket type's currency scalars: each row multiplies one currency's quantity in the costs
 * of a plug inserted into that socket type. The shipped rows name Glimmer on shader and
 * projection sockets and the four Synths on the higher synthesis sockets.
 */
inline constexpr std::size_t kSocketTypeCurrencyScalarDescriptor =
    offsetof(schema::generated::SocketTypeRow, h321354a2);
inline constexpr std::uint32_t kSocketTypeCurrencyScalarClass =
    schema::generated::C80805d86::kClass;
inline constexpr std::size_t kSocketTypeCurrencyScalarStride = sizeof(schema::generated::C80805d86);
/** A scalar row opens with the currency's item index; the next 16 bits are zero in every row. */
inline constexpr std::size_t kCurrencyScalarItemIndexOffset =
    offsetof(schema::generated::C80805d86, gap0);
inline constexpr std::size_t kCurrencyScalarReservedOffset =
    kCurrencyScalarItemIndexOffset + sizeof(std::uint16_t);
/** The multiplier is a float; every shipped value is a whole number of at least one. */
inline constexpr std::size_t kCurrencyScalarValueOffset =
    offsetof(schema::generated::C80805d86, unk4);
static_assert(kCurrencyScalarItemIndexOffset == 0
              && kCurrencyScalarReservedOffset + sizeof(std::uint16_t)
                     == kCurrencyScalarValueOffset);
/** Acquired-state and item-completion arrays carry this postfix expression-row class. */
inline constexpr std::uint32_t kInvestmentExpressionRowClass =
    schema::generated::InvestmentExpressionRow::kClass;
/** Catalyst progress items reference objective rows through this one-row array. */
inline constexpr std::uint32_t kObjectiveReferenceArrayClass =
    schema::generated::ObjectiveReferenceArray::kClass;
/** Class word of an item's plug block; on a catalyst it follows the one-row objective array. */
inline constexpr std::uint32_t kPlugBlockClass = schema::generated::PlugBlock::kClass;
/** Signed 64-bit offset to an item's objective block, relative to the field. */
inline constexpr std::size_t kItemObjectiveBlockOffset =
    offsetof(schema::generated::ItemDefinition, objectiveItemBlock);
/** This block holds objective indices, completion flags and the item that owns the quest set. */
inline constexpr std::uint32_t kItemObjectiveBlockClass =
    schema::generated::ItemObjectiveBlock::kClass;
/** A 16-bit item-table index, not a definition hash. */
inline constexpr std::size_t kObjectiveParentItemOffset =
    offsetof(schema::generated::ItemObjectiveBlock, parentItemIndex);
/** Unlock block offset, relative to the field; zero means absent. */
inline constexpr std::size_t kItemUnlockBlockOffset =
    offsetof(schema::generated::ItemDefinition, unlockItemBlock);
/** This block's first array names the flags supplied by item presence. */
inline constexpr std::uint32_t kItemUnlockBlockClass = schema::generated::ItemUnlockBlock::kClass;
/** Presence-flag entries hold unlock slot indices, not saved bank rows. */
inline constexpr std::uint32_t kItemPresenceFlagClass = schema::generated::ItemPresenceFlag::kClass;
inline constexpr std::size_t kItemPresenceFlagDescriptor =
    offsetof(schema::generated::ItemUnlockBlock, presenceFlags);
/** Each presence-flag entry occupies one 16-bit slot. */
inline constexpr std::size_t kItemPresenceFlagStride = sizeof(schema::generated::ItemPresenceFlag);
inline constexpr std::size_t kItemPresenceFlagSlotOffset =
    offsetof(schema::generated::ItemPresenceFlag, unlockSlot);
/** Objective array entries are 16-bit table indices. */
inline constexpr std::size_t kObjectiveArrayDescriptor =
    offsetof(schema::generated::ItemObjectiveBlock, objectives);
inline constexpr std::size_t kObjectiveReferenceStride =
    sizeof(schema::generated::ObjectiveReferenceArray);
/** The investment root holds the dense objective definition table at this slot. */
inline constexpr std::size_t kObjectiveTableSlot =
    root_slot(offsetof(schema::generated::InvestmentRoot, slot58Objectives));
/** Installed objective table and row layout. */
inline constexpr std::uint32_t kObjectiveTableClass = schema::generated::ObjectiveTable::kClass;
inline constexpr std::uint32_t kObjectiveRowClass = schema::generated::ObjectiveRow::kClass;
/** The objective table holds its rows in this array. */
inline constexpr std::size_t kObjectiveRowArrayDescriptor =
    offsetof(schema::generated::ObjectiveTable, table);
inline constexpr std::size_t kObjectiveRowStride = sizeof(schema::generated::ObjectiveRow);
inline constexpr std::size_t kObjectiveCompletionValueOffset =
    offsetof(schema::generated::ObjectiveRow, completionValue);
/** An objective row's optional and counts-down flags, the two words before its completion value. */
inline constexpr std::size_t kObjectiveOptionalOffset =
    offsetof(schema::generated::ObjectiveRow, gap28);
inline constexpr std::size_t kObjectiveCountsDownOffset =
    kObjectiveOptionalOffset + sizeof(std::uint32_t);
static_assert(kObjectiveCountsDownOffset + sizeof(std::uint32_t)
              == kObjectiveCompletionValueOffset);
/** Expression whose value the native evaluator subtracts from an objective's completion value. */
inline constexpr std::size_t kObjectiveTargetAdjustmentField =
    offsetof(schema::generated::ObjectiveRow, hA32e809c);
/** The investment root holds the socket entry list table at this slot. */
inline constexpr std::size_t kSocketEntryListTableSlot =
    root_slot(offsetof(schema::generated::InvestmentRoot, slot97SocketEntryLists));
/** The globals container child that names the investment root. */
inline constexpr std::size_t kInvestmentRootChild =
    globals_child(offsetof(schema::generated::InvestmentGlobals, investmentRoot));
/** The investment root holds the inventory bucket table at this slot. */
inline constexpr std::size_t kBucketTableSlot =
    root_slot(offsetof(schema::generated::InvestmentRoot, slot17BucketTable));
/**
 * Package class of the bucket-definition table pairing inventory buckets with native equipment
 * slots. Exactly one installed entry carries it, so a class sweep locates the table without
 * naming the tag it happens to hold in one install.
 */
inline constexpr std::uint32_t kBucketDefinitionTableClass =
    schema::generated::BucketDefinitionTable::kClass;
/** The installed table contains one row for each item-bearing bucket definition. */
inline constexpr std::size_t kBucketDefinitionCount = 34;
/** One resolved bucket definition occupies 72 bytes. */
inline constexpr std::size_t kBucketDefinitionSize = sizeof(schema::generated::BucketDefinitionRow);
/** The native equipment-slot byte sits here in each resolved bucket definition. */
inline constexpr std::size_t kBucketDefinitionEquipmentSlotOffset =
    offsetof(schema::generated::BucketDefinitionRow, equipmentSlot);
/** Non-equippable bucket definitions carry the all-one slot sentinel. */
inline constexpr std::uint8_t kBucketDefinitionUnavailableEquipmentSlot = 0xFF;
/** Element class of the socket entry list table. */
inline constexpr std::uint32_t kSocketEntryListTableClass =
    schema::generated::SocketEntryList::kClass;
/** A socket entry list definition holds its entry array descriptor here. */
inline constexpr std::size_t kSocketEntryArrayDescriptor =
    offsetof(schema::generated::SocketEntryListDefinition, entries);
/** One socket entry is 64 bytes. */
inline constexpr std::size_t kSocketEntrySize = sizeof(schema::generated::SocketEntry);
/** A socket entry holds its plug source hash here. */
inline constexpr std::size_t kSocketEntryPlugSource =
    offsetof(schema::generated::SocketEntry, plugSource);
/** Bucket group this socket entry competes in. `0xFF` marks an entry with no group. */
inline constexpr std::size_t kSocketEntryGroup = offsetof(schema::generated::SocketEntry, group);
/** Entry kind. Kind 34 is the super lane, which is active despite carrying no plug source. */
inline constexpr std::size_t kSocketEntryKind = offsetof(schema::generated::SocketEntry, kind);
/** The absent plug source. */
inline constexpr std::uint32_t kNoPlugSource = 0x811C9DC5U;
/** The bucket table declares its descriptor count here. */
inline constexpr std::size_t kBucketCountOffset =
    offsetof(schema::generated::C80807c62, bucketCount);
/** Bucket descriptors follow the count. */
inline constexpr std::size_t kBucketFirstDescriptor =
    offsetof(schema::generated::C80807c62, bucketDescriptors);
/** One bucket descriptor is 36 bytes. */
inline constexpr std::size_t kBucketDescriptorSize = sizeof(schema::generated::G80807c6290);
/** Bucket descriptor field offsets. */
inline constexpr std::size_t kBucketDescriptorIdOffset =
    offsetof(schema::generated::G80807c6290, bucketId);
inline constexpr std::size_t kBucketFirstSlotOffset =
    offsetof(schema::generated::G80807c6290, firstSlot);
inline constexpr std::size_t kBucketSlotCountOffset =
    offsetof(schema::generated::G80807c6290, slotCount);
inline constexpr std::size_t kBucketArraySelectorOffset =
    offsetof(schema::generated::G80807c6290, arraySelector);
/**
 * Acquisition policies in the two bytes after the inventory-array selector, unnamed in the
 * generated layout. Shipped rows hold only 0 or 1: six buckets set the first, among them the
 * 21-row recovery bucket most items name, and the five-row bucket 37 alone sets the second.
 */
inline constexpr std::size_t kBucketFifoOffset = kBucketArraySelectorOffset + 1;
inline constexpr std::size_t kBucketNoTransferOnEvictionOffset = kBucketArraySelectorOffset + 2;
// Pins the offsets the unnamed bytes above were measured at against a regenerated layout.
static_assert(kBucketFifoOffset == 25 && kBucketNoTransferOnEvictionOffset == 26);

/** Installed tags carrying the sobject definition table. The first one that reads is used. */
inline constexpr std::uint32_t kSobjectTablePrimaryTag = 0x81327CD4U;
inline constexpr std::uint32_t kSobjectTableAlternateTag = 0x80B9E5BFU;
/** One sobject row, and the three fields an incident target needs from it. */
inline constexpr std::size_t kSobjectRowStride = sizeof(schema::generated::SobjectRow);
inline constexpr std::size_t kSobjectNameHashOffset =
    offsetof(schema::generated::SobjectRow, nameHash);
/** The record row and the lore ordinal, two bytes each, read as one four-byte lane. */
inline constexpr std::size_t kSobjectLaneOffset =
    offsetof(schema::generated::SobjectRow, recordRow);
inline constexpr std::size_t kSobjectTypeCodeOffset =
    offsetof(schema::generated::SobjectRow, typeCode);
static_assert(offsetof(schema::generated::SobjectRow, loreObjectOrdinal)
              == kSobjectLaneOffset + sizeof(schema::generated::SobjectRow::recordRow));
/** The sobject table root names its row array here. */
inline constexpr std::size_t kSobjectArrayDescriptor =
    offsetof(schema::generated::C80807c9b, incidentGlobals.hAd7fd470);

/**
 * Locates the sobject rows in one table blob.
 * @return False when the array is unreadable or a row falls outside the blob.
 */
[[nodiscard]] inline bool read_sobject_rows(std::span<const std::byte> blob, Array& rows) noexcept {
    return read_array(blob,
                      kSobjectArrayDescriptor,
                      schema::generated::SobjectRow::kClass,
                      kSobjectRowStride,
                      rows);
}

/**
 * Reads one row's type code. It is one signed byte; a wider read turns -1 into 255.
 * @param blob Table blob the row lies in, already bounds checked by `read_sobject_rows`.
 * @param row Offset of the row.
 */
[[nodiscard]] inline std::int32_t sobject_type_code(std::span<const std::byte> blob,
                                                    std::size_t row) noexcept {
    return static_cast<std::int8_t>(blob[row + kSobjectTypeCodeOffset]);
}

/** @param blob Whole container bytes. @return Its child count. */
[[nodiscard]] std::size_t child_count(std::span<const std::byte> blob) noexcept;

/**
 * Reads one row of an index table that was found.
 * @param blob Whole definition bytes owning the array.
 * @param array Array with a 24-byte stride.
 * @param index Row ordinal.
 * @param row Receives the row fields.
 * @return True when the row is inside the blob.
 */
[[nodiscard]] bool index_row(std::span<const std::byte> blob,
                             const Array& array,
                             std::uint64_t index,
                             IndexRow& row) noexcept;

} // namespace sunrise::middleware::content::packages::tables
