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
/** 3 mirrored bytes carry the same preview-availability policy. */
inline constexpr std::size_t kPreviewMirrorCount = 3;
/** Race, gender, and class form the 3 authored identity values. */
inline constexpr std::size_t kIdentityValueCount = 3;
/** The per-character acquired-state bank reserves 4,096 byte flags. */
inline constexpr std::size_t kFlagCapacity = 4096;
/** The per-character objective bank reserves 768 signed values. */
inline constexpr std::size_t kObjectiveValueCapacity = 768;
/** The compact creation header occupies 36 bytes and stays 0 for an existing character. */
inline constexpr std::size_t kCreationHeaderSize = 36;
/** The next inventory serial is followed by 4 reserved alignment bytes. */
inline constexpr std::size_t kInventorySerialPaddingSize = 4;
/** 200 reserved bytes separate inventory rows from equipped instance keys. */
inline constexpr std::size_t kInventoryEquipmentPaddingSize = 200;
/** 52 reserved bytes separate the equipment summary from its validity gate. */
inline constexpr std::size_t kSummaryGatePaddingSize = 52;
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
/** The periodic reset record occupies 56 native bytes and starts empty. */
inline constexpr std::size_t kPeriodicResetRecordSize = 56;
/** 18,224 reserved bytes come before the acquired-state flags. */
inline constexpr std::size_t kResetFlagsPaddingSize = 18224;
/** 1,464 reserved bytes follow objective values. */
inline constexpr std::size_t kValuesContentPaddingSize = 1464;
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

/** Byte-exact selected-character Family-4 object generated from resolved loadout rows. */
struct Object {
    std::uint64_t characterSoid{};
    Identity identity{};
    std::array<std::byte, kCreationHeaderSize> creationHeader{};
    /** Next serial assigned to a newly mutated inventory row; this is not an occupancy count. */
    std::uint32_t nextInventorySerial{};
    std::array<std::byte, kInventorySerialPaddingSize> inventorySerialPadding{};
    std::array<inventory::layout::Entry, kInventoryCapacity> inventoryItems{};
    std::array<std::byte, kInventoryEquipmentPaddingSize> inventoryEquipmentPadding{};
    std::array<std::uint64_t, kEquipmentCapacity> equippedInstanceSoids{};
    EquipmentSummary equipmentSummary{};
    std::array<std::byte, kSummaryGatePaddingSize> summaryGatePadding{};
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
    /** One bit per inventory row, set for every row the loadout occupies. */
    std::array<std::byte, kNewItemFlagByteCount> newItemFlags{};
    /** Per-row acknowledgement watermarks mirrored from the matching item instances. */
    std::array<std::int32_t, kInstanceProgressWatermarkCapacity> instanceProgressWatermarks{};
    std::array<std::byte, kProgressPreviewPaddingSize> progressPreviewPadding{};
    std::array<std::uint8_t, kPreviewMirrorCount> previewMirrors{};
    std::array<std::byte, kPreviewProgressionPaddingSize> previewProgressionPadding{};
    std::array<progression::layout::Entry, kProgressionCapacity> progressions{};
    std::array<std::byte, kPeriodicResetRecordSize> periodicResetRecord{};
    std::array<std::byte, kResetFlagsPaddingSize> resetFlagsPadding{};
    std::array<std::byte, kFlagCapacity> acquiredFlags{};
    std::array<std::int32_t, kObjectiveValueCapacity> objectiveValues{};
    std::array<std::byte, kValuesContentPaddingSize> valuesContentPadding{};
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
static_assert(sizeof(Object) == kObjectSize);
static_assert(std::is_trivially_copyable_v<Object>);

} // namespace sunrise::middleware::datagen::family4::character::layout
