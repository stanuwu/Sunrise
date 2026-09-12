#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::content::packages::tables {

/** Definition tags sit in this closed range. */
inline constexpr std::uint32_t kTagLowerBound = 0x80800000;
/** Values above this are not definition tags. */
inline constexpr std::uint32_t kTagUpperBound = 0x82000000;
/** A tag holds its entry index in the low bits and its package id above them. */
inline constexpr std::uint32_t kTagPackageShift = 13;
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

/** Element class of the item index table inside the investment container. */
inline constexpr std::uint32_t kItemIndexTableClass = 0x80807BE8U;
/** Serialized item definition class, distinct from the item index table class. */
inline constexpr std::uint32_t kItemDefinitionClass = 0x80807BEAU;
/** Investment root slot of the records and lore table. */
inline constexpr std::size_t kRecordTableSlot = 72;
/** One record row, wider than any field this pass reads. */
inline constexpr std::size_t kRecordRowStride = 216;
/** Unlock slot of the record's completion flag, or a non-positive value when it has none. */
inline constexpr std::size_t kRecordCompletionFlagOffset = 100;
/** Authored record definition hash inside one record row. */
inline constexpr std::size_t kRecordHashOffset = 0x28;
/**
 * Lore row a record names, or 0xFFFF when it names none.
 * A collectible row carries the same field at the same offset, which joins the two.
 */
inline constexpr std::size_t kLoreRowOffset = 0x2C;
/** Investment root slot of the lore table. */
inline constexpr std::size_t kLoreTableSlot = 52;
/** One lore row, and the definition hash inside it. */
inline constexpr std::size_t kLoreRowStride = 16;
inline constexpr std::size_t kLoreHashOffset = 8;
/** Points the record is worth. Zero for lore and for the interval records that score per step. */
inline constexpr std::size_t kRecordScoreOffset = 92;
/** A record names its category's value slot here. The record's own bar reads the next slot up. */
inline constexpr std::size_t kRecordCategoryExpressionField = 120;
/** A record's other expression field. A presentation gate sits in either of the two. */
inline constexpr std::size_t kRecordAlternateExpressionField = 136;
/** Objective rows a record names, two bytes each. */
inline constexpr std::size_t kRecordObjectiveField = 48;
inline constexpr std::uint32_t kRecordObjectiveRowClass = 0x80807455U;
inline constexpr std::size_t kRecordObjectiveStride = 2;
/** Interval rows a record names: objective row, score, then the item index the step grants. */
inline constexpr std::size_t kRecordIntervalField = 64;
inline constexpr std::uint32_t kRecordIntervalRowClass = 0x80802C0FU;
inline constexpr std::size_t kRecordIntervalStride = 12;
inline constexpr std::size_t kRecordIntervalObjectiveRowOffset = 0;
inline constexpr std::size_t kRecordIntervalScoreOffset = 4;
inline constexpr std::size_t kRecordIntervalItemIndexOffset = 8;
/** Unlock value slot counting a record's redeemed intervals, or 0xFFFF when it has none. */
inline constexpr std::size_t kRecordRedeemedCountSlotOffset = 82;
/** An objective holds the expression naming its progress source at this field. */
inline constexpr std::size_t kObjectiveSourceExpressionField = 8;
/** Installed tag of the record display table. Its rows sit in record order. */
inline constexpr std::uint32_t kRecordDisplayTableTag = 0x81613CFFU;
inline constexpr std::size_t kRecordDisplayRowStride = 128;
inline constexpr std::uint32_t kRecordDisplayRowClass = 0x80805A99U;
/** Reward rows a record grants, held on its display row: item index then quantity. */
inline constexpr std::size_t kRecordRewardField = 72;
inline constexpr std::uint32_t kRecordRewardRowClass = 0x80805A9BU;
inline constexpr std::size_t kRecordRewardStride = 24;
inline constexpr std::size_t kRecordRewardItemIndexOffset = 0;
inline constexpr std::size_t kRecordRewardQuantityOffset = 4;
/** One item index table row. Row +0 is the item's definition hash. */
inline constexpr std::size_t kItemIndexRowStride = 24;
/** Nonzero when the record's completion grants a character-equippable title. */
inline constexpr std::size_t kRecordHasTitleOffset = 0xB8;
/** Investment root slot of the four unlock value mapping tables. */
inline constexpr std::size_t kUnlockValueMapTableSlot = 113;
/** Array descriptor of the account object's value mapping table. */
inline constexpr std::size_t kAccountValueMapDescriptor = 8;
/** One unlock expression instruction: an opcode then its operand. */
inline constexpr std::size_t kUnlockInstructionStride = 8;
/** The operand within one unlock expression instruction. */
inline constexpr std::size_t kUnlockInstructionOperandOffset = 4;
/** An expression field is a 64-bit count then a 64-bit self-relative offset. */
inline constexpr std::size_t kUnlockExpressionFieldSize = 16;
inline constexpr std::size_t kUnlockExpressionPointerOffset = 8;
/** Opcodes run to fifteen; anything wider means the field is not an expression. */
inline constexpr std::uint32_t kUnlockOpcodeCeiling = 20;
/** The opcode that reads a value slot. */
inline constexpr std::uint32_t kUnlockReadValueOpcode = 10;
/** The opcode that tests a flag. */
inline constexpr std::uint32_t kUnlockReadFlagOpcode = 1;
/** The opcode that pushes a literal. */
inline constexpr std::uint32_t kUnlockLiteralOpcode = 11;
/** The opcode that tests greater than or equal. */
inline constexpr std::uint32_t kUnlockGreaterEqualOpcode = 14;
/** The opcode that inverts the value on top of the stack. Its operand is unused. */
inline constexpr std::uint32_t kUnlockNotOpcode = 2;
/** The opcode that folds the top two values with logical and. */
inline constexpr std::uint32_t kUnlockAndOpcode = 4;
/** The opcode that tests the top two values for equality. */
inline constexpr std::uint32_t kUnlockEqualOpcode = 8;
/** High half every installed array marker and element class carries. */
inline constexpr std::uint32_t kDefinitionClassHigh = 0x8080U;
/** Shift that leaves `kDefinitionClassHigh` from a marker or element class. */
inline constexpr std::uint32_t kDefinitionClassShift = 16;
/** Array payloads begin after a sixteen byte header. */
inline constexpr std::size_t kHeaderSkip = 16;
/** Stops a wild count being walked as an expression. One shipped book carries 59 instructions. */
inline constexpr std::int64_t kNodeExpressionCapacity = 128;
/** Investment root slot of the presentation node table. */
inline constexpr std::size_t kPresentationNodeTableSlot = 63;
/** One node row. */
inline constexpr std::size_t kNodeRowStride = 168;
/** A node's expression sits at one of these two fields, never both. */
inline constexpr std::size_t kNodeExpressionFieldPrimary = 64;
inline constexpr std::size_t kNodeExpressionFieldAlternate = 48;
/** Records a node owns, four bytes each as a row then a gate. */
inline constexpr std::size_t kNodeChildRecordField = 136;
inline constexpr std::size_t kNodeChildRecordStride = 4;
/** Array descriptor of the character object's flag mapping table, sized to that bank. */
inline constexpr std::size_t kCharacterFlagMapDescriptor = 40;
/** Array descriptor of the character object's value mapping table, sized to that bank. */
inline constexpr std::size_t kCharacterValueMapDescriptor = 24;
/** Investment root slot of the unlock flag mapping tables. */
inline constexpr std::size_t kUnlockFlagMapTableSlot = 111;
/** Array descriptor of the account object's flag mapping table. */
inline constexpr std::size_t kAccountFlagMapDescriptor = 8;
/** One mapping row: a source and the slot it feeds. */
inline constexpr std::size_t kUnlockMapRowStride = 8;
/** The destination slot within one mapping row. */
inline constexpr std::size_t kUnlockMapDestinationSlotOffset = 4;

/** The investment root holds the installed collectible definition table at this slot. */
inline constexpr std::size_t kCollectibleTableSlot = 19;
/** Definition class recorded for the installed investment root tag. */
inline constexpr std::uint32_t kInvestmentRootClass = 0x80807D84U;
/** Definition class recorded for the installed collectible table tag. */
inline constexpr std::uint32_t kCollectibleTableClass = 0x8080306DU;
/** Element class recorded by the collectible table's inline array header. */
inline constexpr std::uint32_t kCollectibleRowClass = 0x80803475U;
/** One collectible row in this installed build occupies 184 bytes. */
inline constexpr std::size_t kCollectibleRowStride = 0xB8;
/** Authored DestinyCollectibleDefinition hash inside one collectible row. */
inline constexpr std::size_t kCollectibleHashOffset = 0x28;
/** Native item-definition index granted by one collectible row. */
inline constexpr std::size_t kCollectibleItemIndexOffset = 0x2C;
/** Native material-requirement-set ordinal carried by one collectible row. */
inline constexpr std::size_t kCollectibleMaterialRequirementIndexOffset = 0x9A;
/** The investment root holds the material-requirement-set table at this slot. */
inline constexpr std::size_t kMaterialRequirementTableSlot = 96;
/** Installed material-requirement-set table and row classes. */
inline constexpr std::uint32_t kMaterialRequirementTableClass = 0x80807ACEU;
inline constexpr std::uint32_t kMaterialRequirementSetRowClass = 0x80807AD4U;
inline constexpr std::uint32_t kMaterialRequirementRowClass = 0x80807AD7U;
/** One set row is a hash followed by a self-relative pointer to an array descriptor. */
inline constexpr std::size_t kMaterialRequirementSetRowStride = 0x10;
inline constexpr std::size_t kMaterialRequirementSetHashOffset = 0;
inline constexpr std::size_t kMaterialRequirementSetArrayPointerOffset = 8;
/** One requirement row is item index, quantity, two flags, then a native sentinel. */
inline constexpr std::size_t kMaterialRequirementRowStride = 0x0C;
inline constexpr std::size_t kMaterialRequirementItemIndexOffset = 0;
inline constexpr std::size_t kMaterialRequirementQuantityOffset = 4;
inline constexpr std::size_t kMaterialRequirementDeleteOffset = 8;
inline constexpr std::size_t kMaterialRequirementOmitOffset = 9;
inline constexpr std::size_t kMaterialRequirementSentinelOffset = 10;
/** Plug item definitions name insertion/enabled requirement-set ordinals at fixed offsets. */
inline constexpr std::size_t kInsertionMaterialRequirementSetIndexOffset = 0x1E8;
inline constexpr std::size_t kEnabledMaterialRequirementSetIndexOffset = 0x200;
inline constexpr std::uint16_t kUnavailableMaterialRequirementSetIndex = 0xFFFFU;
/** Element class of an item's ordinary socket array. */
inline constexpr std::uint32_t kOrdinarySocketClass = 0x808077C4U;
/** One ordinary socket entry is 80 bytes. */
inline constexpr std::size_t kSocketEntryStride = 80;
/** An item definition holds its socket block self-relative at this offset. */
inline constexpr std::size_t kSocketBlockOffset = 104;
/** An item definition holds its inventory bucket id at this offset. */
inline constexpr std::size_t kBucketIdOffset = 184;
/** An item definition holds its art block self-relative at this offset, zero when absent. */
inline constexpr std::size_t kArtBlockOffset = 136;
/** Element class of an item's art row array. */
inline constexpr std::uint32_t kArtRowClass = 0x808077B5U;
/** Element class of an item's material override array. */
inline constexpr std::uint32_t kMaterialOverrideClass = 0x808077B3U;
/** Element class of an item's sandbox perk array. */
inline constexpr std::uint32_t kSandboxPerkClass = 0x808077BCU;
/** An item definition holds its stat block self-relative at this offset, zero when absent. */
inline constexpr std::size_t kStatBlockOffset = 112;
/** The investment root holds the constants blob at this slot. */
inline constexpr std::size_t kInvestmentConstantsSlot = 11;

/** The investment root holds the progression definition table at this slot. */
inline constexpr std::size_t kProgressionTableSlot = 68;
/** Element class of the progression definition table. */
inline constexpr std::uint32_t kProgressionTableClass = 0x80807CDDU;
/** One progression definition is 88 inline bytes. */
inline constexpr std::size_t kProgressionRowStride = 88;
/** A progression definition names the object array holding its record here. */
inline constexpr std::size_t kProgressionScopeOffset = 4;
/** Rank steps a progression declares: the experience each rank costs, in rank order. */
inline constexpr std::size_t kProgressionStepField = 24;
inline constexpr std::uint32_t kProgressionStepRowClass = 0x80807CEDU;
inline constexpr std::size_t kProgressionStepStride = 8;
/** Reward rows a progression grants, held on the progression definition. */
inline constexpr std::size_t kProgressionRewardField = 72;
inline constexpr std::uint32_t kProgressionRewardRowClass = 0x80802FF7U;
inline constexpr std::size_t kProgressionRewardStride = 56;
/** Reward row fields: the rank it needs, the item it grants, and how many. */
inline constexpr std::size_t kProgressionRewardRankOffset = 0;
inline constexpr std::size_t kProgressionRewardItemIndexOffset = 4;
inline constexpr std::size_t kProgressionRewardQuantityOffset = 8;
/** Unlock flag slot a reward's claim sets. The slots run dense in reward order. */
inline constexpr std::size_t kProgressionRewardClaimSlotOffset = 20;
/** Items a wrapper item opens into, two bytes each as an item-definition index. */
inline constexpr std::size_t kGearsetItemField = 392;
inline constexpr std::uint32_t kGearsetItemRowClass = 0x808087DBU;
inline constexpr std::size_t kGearsetItemStride = 2;

/** Expression that makes one collectible acquired, which a purchase or a grant sets. */
inline constexpr std::size_t kCollectibleAcquiredExpressionField = 112;
/** Investment root slot of the unlock flag slot table, which is keyed by slot. */
inline constexpr std::size_t kUnlockFlagSlotTableSlot = 112;
/** One flag slot row, and the bank row it feeds inside the object its kind names. */
inline constexpr std::size_t kUnlockSlotRowStride = 8;
inline constexpr std::size_t kUnlockSlotBankIndexOffset = 6;

/** Installed tag of the per-item strings index table. Its rows sit in item index order. */
inline constexpr std::uint32_t kItemStringsIndexTag = 0x81613CF1U;
inline constexpr std::uint32_t kItemStringsIndexRowClass = 0x80805CDFU;
/** Item-type pair on one item's strings blob: the bank, then the name hash inside it. */
inline constexpr std::size_t kItemStringsTypePairOffset = 144;
inline constexpr std::size_t kItemStringsTypeHashOffset = 148;

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

/** Visitor called once per index-table row. */
using RowVisitor = bool (*)(void* context, std::uint32_t index, const IndexRow& row) noexcept;

/**
 * Reads the array descriptor at one known offset.
 * @param blob Whole definition bytes.
 * @param descriptorOffset Offset of the count and self-relative offset pair.
 * @param output Receives the array that was found.
 * @return True when the descriptor points at a valid array header.
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
inline constexpr std::size_t kItemTableSlot = 48;
/** The investment root holds the shared reusable/randomized plug-set table at this slot. */
inline constexpr std::size_t kPlugSetTableSlot = 51;
/** The investment root holds the dense ordinary socket-type table at this slot. */
inline constexpr std::size_t kSocketTypeTableSlot = 94;
/** Element class and row size of the installed ordinary socket-type table. */
inline constexpr std::uint32_t kSocketTypeTableClass = 0x80807ABBU;
inline constexpr std::size_t kSocketTypeRowStride = 96;
/** One socket type points to its acquired-state rule array from this row offset. */
inline constexpr std::size_t kSocketTypeAcquisitionDescriptor = 16;
/** Acquired-state and item-completion arrays carry this postfix expression-row class. */
inline constexpr std::uint32_t kInvestmentExpressionRowClass = 0x80807D31U;
/** Catalyst progress items reference objective rows through this one-row array. */
inline constexpr std::uint32_t kObjectiveReferenceArrayClass = 0x808087B1U;
inline constexpr std::uint32_t kObjectiveReferenceRowClass = 0x808077E3U;
/** The investment root holds the dense objective definition table at this slot. */
inline constexpr std::size_t kObjectiveTableSlot = 58;
/** Installed objective table and row layout. */
inline constexpr std::uint32_t kObjectiveTableClass = 0x8080775BU;
inline constexpr std::uint32_t kObjectiveRowClass = 0x8080775FU;
inline constexpr std::size_t kObjectiveRowStride = 0xA0;
inline constexpr std::size_t kObjectiveCompletionValueOffset = 0x30;
/** The investment root holds the socket entry list table at this slot. */
inline constexpr std::size_t kSocketEntryListTableSlot = 97;
/** The globals container names the investment root as its first child. */
inline constexpr std::size_t kInvestmentRootChild = 0;
/** The investment root holds the inventory bucket table at this slot. */
inline constexpr std::size_t kBucketTableSlot = 17;
/**
 * Package class of the bucket-definition table pairing inventory buckets with native equipment
 * slots. Exactly one installed entry carries it, so a class sweep locates the table without
 * naming the tag it happens to hold in one install.
 */
inline constexpr std::uint32_t kBucketDefinitionTableClass = 0x80805936U;
/** The installed table contains one row for each item-bearing bucket definition. */
inline constexpr std::size_t kBucketDefinitionCount = 34;
/** One resolved bucket definition occupies 72 bytes. */
inline constexpr std::size_t kBucketDefinitionSize = 72;
/** The native equipment-slot byte sits here in each resolved bucket definition. */
inline constexpr std::size_t kBucketDefinitionEquipmentSlotOffset = 64;
/** Non-equippable bucket definitions carry the all-one slot sentinel. */
inline constexpr std::uint8_t kBucketDefinitionUnavailableEquipmentSlot = 0xFF;
/** Element class of the socket entry list table. */
inline constexpr std::uint32_t kSocketEntryListTableClass = 0x80807A7EU;
/** A socket entry list definition holds its entry array descriptor here. */
inline constexpr std::size_t kSocketEntryArrayDescriptor = 16;
/** One socket entry is 64 bytes. */
inline constexpr std::size_t kSocketEntrySize = 64;
/** A socket entry holds its plug source hash here. */
inline constexpr std::size_t kSocketEntryPlugSource = 8;
/** Bucket group this socket entry competes in. `0xFF` marks an entry with no group. */
inline constexpr std::size_t kSocketEntryGroup = 12;
/** Entry kind. Kind 34 is the super lane, which is active despite carrying no plug source. */
inline constexpr std::size_t kSocketEntryKind = 13;
/** The absent plug source. */
inline constexpr std::uint32_t kNoPlugSource = 0x811C9DC5U;
/** The bucket table declares its descriptor count here. */
inline constexpr std::size_t kBucketCountOffset = 140;
/** Bucket descriptors follow the count. */
inline constexpr std::size_t kBucketFirstDescriptor = 144;
/** One bucket descriptor is 36 bytes. */
inline constexpr std::size_t kBucketDescriptorSize = 36;
/** Bucket descriptor field offsets. */
inline constexpr std::size_t kBucketFirstSlotOffset = 4;
inline constexpr std::size_t kBucketSlotCountOffset = 8;
inline constexpr std::size_t kBucketArraySelectorOffset = 24;

/** Every definition table holds its array descriptor at this fixed offset. */
inline constexpr std::size_t kTableArrayDescriptor = 8;

/** Installed tags carrying the sobject definition table. The first one that reads is used. */
inline constexpr std::uint32_t kSobjectTablePrimaryTag = 0x81327CD4U;
inline constexpr std::uint32_t kSobjectTableAlternateTag = 0x80B9E5BFU;
/** The sobject table declares its row count here, and its rows begin after it. */
inline constexpr std::size_t kSobjectCountOffset = 112;
inline constexpr std::size_t kSobjectFirstRow = 128;
/** One sobject row, and the three fields an incident target needs from it. */
inline constexpr std::size_t kSobjectRowStride = 40;
inline constexpr std::size_t kSobjectNameHashOffset = 0;
inline constexpr std::size_t kSobjectLaneOffset = 16;
inline constexpr std::size_t kSobjectTypeCodeOffset = 36;

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
