#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "../inventory/layout.h"
#include "../progression/layout.h"

namespace sunrise::middleware::datagen::family4::character::layout {

/** The character inventory reserves 350 fixed native rows. */
inline constexpr std::size_t kInventoryCapacity = 350;
/** The equipped-instance and summary arrays each reserve 20 native slots. */
inline constexpr std::size_t kEquipmentCapacity = 20;
/** 16 replicated bytes cover every per-character seen-message bit. */
inline constexpr std::size_t kSeenMessageByteCount = 16;
/** The character roster mirror reserves 20 fixed member rows. */
inline constexpr std::size_t kRosterMirrorCapacity = 20;
/** One opaque roster-mirror member row occupies 144 native bytes. */
inline constexpr std::size_t kRosterMirrorEntrySize = 144;
/** Per-character new-item flags occupy 44 bitmap bytes for all 350 inventory slots. */
inline constexpr std::size_t kNewItemFlagByteCount = 44;
/** One instance-progress watermark accompanies every character inventory slot. */
inline constexpr std::size_t kInstanceProgressWatermarkCapacity = kInventoryCapacity;
/** The character progression bank reserves 127 fixed native rows. */
inline constexpr std::size_t kProgressionCapacity = 127;
/** 3 preview policy bytes. TODO: source each separately once its own reader is known. */
inline constexpr std::size_t kPreviewMirrorCount = 3;
/** Race, gender, and class form the 3 authored identity values. */
inline constexpr std::size_t kIdentityValueCount = 3;
/** The per-character acquired-state bank reserves 4,096 byte flags. */
inline constexpr std::size_t kFlagCapacity = 4096;
/** The per-character objective bank reserves 768 signed values. */
inline constexpr std::size_t kObjectiveValueCapacity = 768;
/** The customisation header occupies 36 bytes. Zero selects row 0, not absence, in every lane. */
inline constexpr std::size_t kCustomisationHeaderSize = 36;
/** The inventory row count is followed by 4 reserved alignment bytes. */
inline constexpr std::size_t kInventoryRowCountPaddingSize = 4;
/** 4 opaque bytes precede the character inventory-change list. */
inline constexpr std::size_t kInventoryChangeUnknownSize = 4;
/** The character object carries at most 16 transient inventory-change records. */
inline constexpr std::size_t kInventoryChangeRecordCapacity = 16;
/** 36 reserved bytes separate the equipment summary from the equipped-title row. */
inline constexpr std::size_t kSummaryTitlePaddingSize = 36;
/** 14 reserved bytes separate the equipped-title row from the inventory validity gate. */
inline constexpr std::size_t kTitleGatePaddingSize = 14;
/** 14 reserved bytes separate the two inventory validity gate fields. */
inline constexpr std::size_t kGateStatePaddingSize = 14;
/** 16 reserved bytes separate the second gate from seen-message storage. */
inline constexpr std::size_t kGateSeenPaddingSize = 16;
/** 28 reserved bytes separate seen messages from the roster mirror. */
inline constexpr std::size_t kSeenRosterPaddingSize = 28;
/** 176 reserved bytes separate the roster mirror from Orbit state. */
inline constexpr std::size_t kRosterDestinationPaddingSize = 176;
/** 792 reserved bytes separate progress watermarks from preview mirrors. */
inline constexpr std::size_t kProgressPreviewPaddingSize = 792;
/** 5 reserved bytes align progression rows after the 3 preview mirrors. */
inline constexpr std::size_t kPreviewProgressionPaddingSize = 5;
/** The periodic reset record occupies 56 native bytes and repeats the family-three block. */
inline constexpr std::size_t kPeriodicResetRecordSize = 56;
/** The character item-stack table reserves 64 fixed native rows. */
inline constexpr std::size_t kItemStackCapacity = 64;
/** One native item-stack row occupies 200 bytes. */
inline constexpr std::size_t kItemStackRowSize = 200;
/** The bytes after the row selector carry quantity, mode, and identity leaves. */
inline constexpr std::size_t kItemStackRowReservedSize = kItemStackRowSize - sizeof(std::int16_t);
/** 4 reserved bytes align the item-stack rows after the next-sequence value. */
inline constexpr std::size_t kItemStackHeaderPaddingSize = 4;
/** 5,416 reserved bytes separate the item-stack rows from the acquired-state flags. */
inline constexpr std::size_t kItemStackTailPaddingSize = 5416;
/** 1,020 reserved bytes separate objective values from the previous-activity index. */
inline constexpr std::size_t kValuesActivityPaddingSize = 1020;
/** 438 reserved bytes separate the travelling-activity index from the content-bypass byte. */
inline constexpr std::size_t kActivityTailPaddingSize = 438;
/** 591 reserved bytes follow the content-bypass policy byte. */
inline constexpr std::size_t kContentTailPaddingSize = 591;
/** The runtime Family-4 character schema owns exactly 46,928 bytes. */
inline constexpr std::size_t kObjectSize = 46928;
/** The equipment summary ends with 3 signed aggregate lanes. */
inline constexpr std::size_t kSummaryIntegerCount = 3;
/** The equipment summary ends with 2 floating-point aggregate lanes. */
inline constexpr std::size_t kSummaryScalarCount = 2;
/** 2 definition-index words come before each equipment-summary score. */
inline constexpr std::size_t kSummaryDefinitionWordCount = 2;
/** Profile and character form the 2 equipment-summary arrays. */
inline constexpr std::size_t kSummaryArrayCount = 2;
/** Every set bit is the native absent definition index in an equipment-summary slot. */
inline constexpr std::uint16_t kEmptySummaryDefinitionIndex = 0xFFFF;
/** The Seals screen compares its record row against this field. */
inline constexpr std::size_t kEquippedTitleRecordIndexOffset = 11'992;

#pragma pack(push, 1)

/** Stable race, gender, and class values preceding the optional creation header. */
struct Identity {
    std::int8_t race{};
    std::int8_t gender{};
    std::int8_t characterClass{};
    std::uint8_t reserved{};
};

/** One definition-and-score pair in the stored equipment summary. */
struct EquipmentSummaryEntry {
    std::uint16_t definitionIndex{};
    std::uint16_t reserved{};
    std::int32_t score{};
};

/** Stored equipment summary consumed before any live inventory recomputation. */
struct EquipmentSummary {
    std::array<EquipmentSummaryEntry, kEquipmentCapacity> profile{};
    std::array<EquipmentSummaryEntry, kEquipmentCapacity> character{};
    std::int32_t divisor{};
    std::int32_t total{};
    std::int32_t light{};
    float lightScalar{};
    float damageScalar{};
};

/** One opaque zero-key roster-mirror row reserved for runtime member sync. */
struct RosterMirrorEntry {
    std::array<std::byte, kRosterMirrorEntrySize> bytes{};
};

/** One transient inventory mutation consumed by the native character-object observer. */
struct InventoryChangeRecord {
    /** Rising record identity; the native observer prefers the greatest matching value. */
    std::uint16_t sequence{};
    std::uint16_t reserved{};
    /** Mutation serial of the inventory row this record describes. */
    std::int32_t mutationSerial{};
    /** Nonzero mutation kind. Kind 1 follows the ordinary item-acquisition path. */
    std::uint8_t kind{};
    std::uint8_t reservedKind{};
    /** Native observer policy bits; 0 enables the ordinary acquisition path. */
    std::uint16_t flags{};
};

/** One fixed item-stack row. Only the leading selector carries a native empty sentinel. */
struct ItemStackRow {
    /** -1 marks the row unused. 0 is a real selector, so it cannot mean empty. */
    std::int16_t selector{};
    std::array<std::byte, kItemStackRowReservedSize> reserved{};
};

/** Header and fixed record bank occupying bytes 0x2BFC through 0x2CBF. */
struct InventoryChangeList {
    /** Ring slot the native producer will write next. */
    std::uint16_t writeSlot{};
    /** Sequence assigned to the next native record. */
    std::uint16_t nextSequence{};
    std::array<InventoryChangeRecord, kInventoryChangeRecordCapacity> records{};
};

/** Byte-exact selected-character Family-4 object generated from resolved loadout rows. */
struct Object {
    std::uint64_t characterSoid{};
    Identity identity{};
    /** Same header and render consumer as the family-three character record. */
    std::array<std::byte, kCustomisationHeaderSize> customisationHeader{};
    /**
     * Length of the inventory prefix the native render walk covers. Rows are placed inside
     * per-bucket ranges, so this is the highest occupied row plus one, not the item count.
     */
    std::uint32_t inventoryRowCount{};
    std::array<std::byte, kInventoryRowCountPaddingSize> inventoryRowCountPadding{};
    std::array<inventory::layout::Entry, kInventoryCapacity> inventoryItems{};
    std::array<std::byte, kInventoryChangeUnknownSize> inventoryChangeUnknown{};
    InventoryChangeList inventoryChanges{};
    std::array<std::uint64_t, kEquipmentCapacity> equippedInstanceSoids{};
    EquipmentSummary equipmentSummary{};
    std::array<std::byte, kSummaryTitlePaddingSize> summaryTitlePadding{};
    /** Native DestinyRecordDefinition row of the equipped title, or 0xFFFF. */
    std::uint16_t equippedTitleRecordIndex{kEmptySummaryDefinitionIndex};
    std::array<std::byte, kTitleGatePaddingSize> titleGatePadding{};
    /** 0 is a valid definition index and keeps the inventory-present gate open. */
    std::uint16_t inventoryGateDefinitionIndex{};
    std::array<std::byte, kGateStatePaddingSize> gateStatePadding{};
    /** 0 is the required starting inventory gate state. */
    std::uint32_t inventoryGateState{};
    std::array<std::byte, kGateSeenPaddingSize> gateSeenPadding{};
    std::array<std::byte, kSeenMessageByteCount> seenMessages{};
    std::array<std::byte, kSeenRosterPaddingSize> seenRosterPadding{};
    std::array<RosterMirrorEntry, kRosterMirrorCapacity> rosterMirror{};
    std::array<std::byte, kRosterDestinationPaddingSize> rosterDestinationPadding{};
    std::uint32_t lastOrbitedDestination{};
    /** One bit per inventory row, set while the item is unseen. */
    std::array<std::byte, kNewItemFlagByteCount> newItemFlags{};
    /** Per-row acknowledgement watermarks mirrored from the matching item instances. */
    std::array<std::int32_t, kInstanceProgressWatermarkCapacity> instanceProgressWatermarks{};
    std::array<std::byte, kProgressPreviewPaddingSize> progressPreviewPadding{};
    std::array<std::uint8_t, kPreviewMirrorCount> previewMirrors{};
    std::array<std::byte, kPreviewProgressionPaddingSize> previewProgressionPadding{};
    std::array<progression::layout::Entry, kProgressionCapacity> progressions{};
    /** Carries the daily and weekly reset stamps; the encoder writes it as a PeriodicReset. */
    std::array<std::byte, kPeriodicResetRecordSize> periodicResetRecord{};
    /** Sequence handed to the next created item stack; 0 is correct for a new character. */
    std::int32_t nextItemStackSequence{};
    std::array<std::byte, kItemStackHeaderPaddingSize> itemStackHeaderPadding{};
    std::array<ItemStackRow, kItemStackCapacity> itemStacks{};
    std::array<std::byte, kItemStackTailPaddingSize> itemStackTailPadding{};
    std::array<std::byte, kFlagCapacity> acquiredFlags{};
    std::array<std::int32_t, kObjectiveValueCapacity> objectiveValues{};
    std::array<std::byte, kValuesActivityPaddingSize> valuesActivityPadding{};
    /**
     * The activity before the character's current one. The client seeds its fireteam's previous
     * activity from it and takes it as a launch's from side, so an onward link keeps its seamless
     * transition only when it names the activity being left. 0xFFFF means none; 0 is the orbit.
     */
    std::uint16_t previousActivityIndex{};
    /**
     * Overrides the previous activity as the transition's from side when not 0xFFFF.
     * 0 is the orbit, not absent.
     */
    std::uint16_t activityOverrideIndex{};
    /**
     * The activity whose orbit bubble the character is in. The client resolves it to an orbit
     * slice set and compares that against the launched activity's, and a match selects the
     * arrival fly-in. It reads this once, before the launch, so it must already be published.
     */
    std::uint16_t currentActivityIndex{};
    std::array<std::byte, kActivityTailPaddingSize> activityTailPadding{};
    /** This policy byte is effective only with the matching family-five gate arm. */
    std::uint8_t contentBypass{};
    std::array<std::byte, kContentTailPaddingSize> contentTailPadding{};
};

#pragma pack(pop)

static_assert(sizeof(Identity) == kIdentityValueCount * sizeof(std::int8_t) + sizeof(std::uint8_t));
static_assert(sizeof(EquipmentSummaryEntry)
              == kSummaryDefinitionWordCount * sizeof(std::uint16_t) + sizeof(std::int32_t));
static_assert(sizeof(EquipmentSummary)
              == kSummaryArrayCount * kEquipmentCapacity * sizeof(EquipmentSummaryEntry)
                     + kSummaryIntegerCount * sizeof(std::int32_t)
                     + kSummaryScalarCount * sizeof(float));
static_assert(sizeof(InventoryChangeRecord)
              == 3 * sizeof(std::uint16_t) + sizeof(std::int32_t) + 2 * sizeof(std::uint8_t));
static_assert(offsetof(InventoryChangeRecord, mutationSerial) == 2 * sizeof(std::uint16_t));
static_assert(offsetof(InventoryChangeRecord, kind)
              == 2 * sizeof(std::uint16_t) + sizeof(std::int32_t));
static_assert(offsetof(InventoryChangeRecord, flags)
              == 2 * sizeof(std::uint16_t) + sizeof(std::int32_t) + 2 * sizeof(std::uint8_t));
static_assert(sizeof(InventoryChangeList)
              == 2 * sizeof(std::uint16_t)
                     + kInventoryChangeRecordCapacity * sizeof(InventoryChangeRecord));
static_assert(offsetof(InventoryChangeList, records) == 2 * sizeof(std::uint16_t));
static_assert(sizeof(ItemStackRow) == kItemStackRowSize);
static_assert(sizeof(Object) == kObjectSize);
static_assert(offsetof(Object, equippedTitleRecordIndex) == kEquippedTitleRecordIndexOffset);
static_assert(std::is_trivially_copyable_v<Object>);

} // namespace sunrise::middleware::datagen::family4::character::layout
