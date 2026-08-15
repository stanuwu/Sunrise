#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../content/content_catalog.h"
#include "../../abilities/definition.h"
#include "../../constants/definition.h"
#include "../../definition.h"
#include "../../hash_names/definition.h"
#include "../../items/details/definition.h"
#include "../../items/item_catalog.h"
#include "../../progressions/definition.h"
#include "../../scenarios/definition.h"
#include "../../spawn_sets/definition.h"

namespace sunrise::state::build_data::cache::records {

/** These 8 ASCII bytes mark a Sunrise build-data file. */
inline constexpr std::array<char, 8> kCacheMagic{'S', 'U', 'N', 'R', 'I', 'S', 'E', 'B'};
/**
 * Current build-data cache format. An older cache is rebuilt rather than read, so a bump needs
 * no other edit. Bump it whenever a domain's stored shape changes.
 */
inline constexpr std::uint32_t kCacheFormatVersion = 23;
/** Signed -1 on disk means there is no equipment slot. */
inline constexpr std::int8_t kAbsentEquipmentSlot = -1;
/** The standard 64-bit FNV-1a offset basis starts the payload checksum. */
inline constexpr std::uint64_t kChecksumOffsetBasis = 14695981039346656037ULL;
/** The standard 64-bit FNV-1a prime mixes in every written payload byte. */
inline constexpr std::uint64_t kChecksumPrime = 1099511628211ULL;

#pragma pack(push, 1)
/** Version prefix shared by every known cache format. */
struct Prefix {
    std::array<char, kCacheMagic.size()> magic{};
    std::uint32_t version{};
};

/**
 * Stat rows named by the installed investment constants blob.
 * The client searches the character's stat table by these rows, so they decide which rows the
 * generated table may carry. They are 7 bytes of scalars, so they ride in the header.
 */
struct InvestmentConstants {
    /** Stat row the banner's power number is searched by. Row 0 is a real row, so there is no
     * unset value; an unextracted blob leaves `extracted` clear instead. */
    std::uint8_t lightStatRow{};
    std::array<std::uint8_t, constants::kCharacterStatRowCount> characterStatRows{};
    /** One when the constants blob was read, zero when the domain has never been extracted. */
    std::uint8_t extracted{};
};

/** Fixed header preceding every record array. */
struct Header {
    std::array<char, kCacheMagic.size()> magic{};
    std::uint32_t version{};
    std::uint32_t imageTimestamp{};
    std::uint32_t imageSize{};
    std::uint64_t configuredEquipmentHash{};
    std::uint32_t namedCount{};
    std::uint32_t itemCount{};
    std::uint32_t itemDetailCount{};
    std::uint32_t inventoryBucketCount{};
    std::uint32_t socketEntryListCount{};
    std::uint32_t socketEntryTableCount{};
    std::uint32_t abilityBucketCount{};
    std::uint32_t progressionCount{};
    std::uint32_t scenarioCount{};
    std::uint32_t rosterGroupCount{};
    std::uint32_t spawnStemCount{};
    std::uint32_t spawnNameHashCount{};
    std::uint32_t hashNameCount{};
    InvestmentConstants constants{};
    std::uint64_t payloadChecksum{};
};

/** Disk form of one named mapping. Its length field has a fixed width. */
struct NamedRecord {
    std::array<char, content::kDefinitionNameCapacity> name{};
    std::uint16_t nameLength{};
    /** Must be zero, so every unused disk byte always matches. */
    std::uint16_t reserved{};
    std::uint32_t tag{};
    std::uint32_t classId{};
};

/** Disk form of one installed-build item definition mapping. */
struct ItemRecord {
    std::uint32_t definitionHash{};
    std::uint16_t definitionIndex{};
    std::uint8_t bucketId{items::kUnresolvedBucketId};
    /** Must be zero, so the packed item row matches across compilers. */
    std::uint8_t reserved{};
};

/** Disk form of the configured item fields instance generation uses. */
struct ItemDetailRecord {
    std::uint16_t definitionIndex{};
    std::uint8_t bucketId{};
    std::int8_t equipmentSlot{kAbsentEquipmentSlot};
    /** 0 and 1 store the stackable or instanced flag. */
    std::uint8_t instancedDefinition{};
    std::uint8_t ordinarySocketState{};
    std::uint8_t ordinarySocketCount{};
    std::int32_t maxStackSize{};
    std::uint16_t socketEntryListIndex{};
    std::array<std::uint16_t, items::details::kInitialPlugCapacity> initialPlugIndices{};
    std::array<std::uint16_t, items::details::kInitialPlugCapacity> socketTypes{};
    /** Declared per-item stat contributions, extracted from the package definition blob. */
    std::uint8_t statCount{};
    std::array<std::uint8_t, items::details::kStatCapacity> statRows{};
    std::array<std::int32_t, items::details::kStatCapacity> statValues{};
    std::uint32_t definitionHash{};
    std::uint16_t gearArtIndex{};
    std::uint16_t artArrangementIndex{};
    std::uint8_t sandboxPerkCount{};
    std::array<std::uint16_t, items::details::kSandboxPerkCapacity> sandboxPerks{};
    /** Material override rows in the stage order the character record folds them in. */
    std::uint8_t renderOverrideCount{};
    std::array<std::uint8_t, items::details::kRenderOverrideCapacity> overrideStages{};
    std::array<std::int8_t, items::details::kRenderOverrideCapacity> overrideKeys{};
    std::array<std::uint16_t, items::details::kRenderOverrideCapacity> overrideValues{};
};

/** Disk form of one inventory-bucket array-routing descriptor. */
struct InventoryBucketRecord {
    std::uint8_t bucketId{};
    std::uint8_t arraySelector{};
    std::uint16_t firstSlot{};
    std::uint16_t slotCount{};
};

/** Disk form of the buckets one subclass publishes under one ability selection. */
struct AbilityBucketRecord {
    std::uint16_t socketEntryListIndex{};
    std::uint8_t movementEntry{};
    std::uint8_t grenadeEntry{};
    std::uint8_t superEntry{};
    std::uint8_t meleeEntry{};
    std::uint8_t classEntry{};
    std::uint8_t overflowCount{};
    std::array<std::uint8_t, abilities::kBucketCapacity> bucketKinds{};
    std::array<std::uint8_t, abilities::kBucketCapacity> bucketHashCounts{};
    std::array<std::uint32_t, abilities::kBucketCapacity * abilities::kBucketHashCapacity>
        bucketHashes{};
    std::array<std::uint32_t, abilities::kOverflowCapacity> overflow{};
};

/** Disk form of one progression definition and the object array it routes to. */
struct ProgressionRecord {
    std::uint16_t definitionIndex{};
    std::uint8_t scope{};
    /** Must be zero, so the packed progression row always matches. */
    std::uint8_t reserved{};
};

/** Disk form of one dense socket-entry-list definition. */
struct SocketEntryListRecord {
    std::uint32_t definitionHash{};
    std::uint16_t definitionIndex{};
    std::uint8_t entryCount{};
    /** Must be zero, so the packed socket-list row always matches. */
    std::uint8_t reserved{};
    std::uint64_t readyMask{};
};

/** One list's per-entry selection inputs, stored only for lists that carry a super lane. */
struct SocketEntryTableRecord {
    std::uint16_t definitionIndex{};
    /** Must be zero, so the packed row always matches. */
    std::array<std::uint8_t, 2> reserved{};
    /** Plug source per entry, which decides which entries a selection makes active. */
    std::array<std::uint32_t, socket_entry_lists::kEntryCapacity> plugSources{};
    /** Competition group per entry. */
    std::array<std::uint8_t, socket_entry_lists::kEntryCapacity> groups{};
    /** Entry kind per entry. Kind 34 is the super lane. */
    std::array<std::uint8_t, socket_entry_lists::kEntryCapacity> kinds{};
};

/** Disk form of one destination's extracted bubble layout and roster groups. */
struct ScenarioRecord {
    std::array<char, scenarios::kNameCapacity> name{};
    std::uint32_t tag{};
    std::uint8_t nameLength{};
    std::uint8_t bubbleCount{};
    std::uint8_t truncated{};
    std::uint8_t rosterGroupCount{};
    /** Map-package stem the destination's spawn sets are grouped under. */
    std::uint8_t spawnStemLength{};
    /** Must be zero, so the packed destination row always matches. */
    std::array<std::uint8_t, 3> reserved{};
    std::array<char, scenarios::kSpawnStemCapacity> spawnStem{};
    std::array<std::uint8_t, scenarios::kBubbleCapacity> bubbleStates{};
    /** Each bubble's own name hash, in the same order as the states. */
    std::array<std::uint32_t, scenarios::kBubbleCapacity> bubbleHashes{};
    /** Slice-set states each bubble declares, in the same order as the states. */
    std::array<std::uint8_t, scenarios::kBubbleCapacity> bubbleStateCounts{};
    /** Roster table indices, in publish order. */
    std::array<std::uint16_t, scenarios::kDestinationGroupCapacity> rosterGroups{};
    /** Each bubble's map-global index, which spawn-set bubble masks are keyed by. */
    std::array<std::uint16_t, scenarios::kBubbleCapacity> bubbleMapIndices{};
    /** Packages this destination loads, from its slice-set entries and its own tag. */
    std::uint8_t packageCount{};
    /** Must be zero, so the packed destination row always matches. */
    std::uint8_t packageReserved{};
    std::array<std::uint16_t, scenarios::kDestinationPackageCapacity> packages{};
};

/** Disk form of one map-package stem and the spawn-name hash range it owns. */
struct SpawnStemRecord {
    std::array<char, spawn_sets::kStemNameCapacity> name{};
    std::uint32_t pointCount{};
    std::uint16_t setCount{};
    std::uint16_t nameHashOffset{};
    std::uint16_t nameHashCount{};
    std::uint8_t nameLength{};
    /** Must be zero, so the packed stem row always matches. */
    std::uint8_t reserved{};
};

/** Disk form of one resolved bubble name. */
struct HashNameRecord {
    std::array<char, hash_names::kNameLength> name{};
    std::uint32_t hash{};
    std::uint8_t nameLength{};
    /** Must be zero, so the packed bubble-name row always matches. */
    std::array<std::uint8_t, 3> reserved{};
};

/** Disk form of one distinct spawn-name hash inside a map-package stem. */
struct SpawnNameHashRecord {
    std::uint32_t value{};
    std::uint32_t pointCount{};
    std::uint16_t stemIndex{};
    /** Must be zero, so the packed hash row always matches. */
    std::array<std::uint8_t, 2> reserved{};
    /** Bubbles that offer this set, as one bit per map-global bubble index. */
    std::array<std::uint8_t, spawn_sets::kBubbleMaskBytes> bubbleMask{};
    /** One when a tag declaring this hash has no owning container, so it carries no mask. */
    std::uint8_t unbound{};
    /** One when a map package declares it, so every destination of the stem loads it. */
    std::uint8_t inMapPackage{};
    std::uint8_t activityPackageCount{};
    std::uint8_t activityPackageOverflow{};
    std::array<std::uint16_t, spawn_sets::kPackageCapacity> activityPackages{};
};

/** Disk form of one roster group object and its slots. */
struct RosterGroupRecord {
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint16_t slotCount{};
    std::array<std::uint8_t, scenarios::kRosterSlotCapacity> slotTypes{};
    std::array<std::uint8_t, scenarios::kRosterSlotCapacity> slotFlags{};
};

#pragma pack(pop)

static_assert(sizeof(Prefix) == kCacheMagic.size() + sizeof(std::uint32_t));
static_assert(sizeof(InvestmentConstants)
              == constants::kCharacterStatRowCount + 2 * sizeof(std::uint8_t));
static_assert(sizeof(Header)
              == kCacheMagic.size() + 16 * sizeof(std::uint32_t) + 2 * sizeof(std::uint64_t)
                     + sizeof(InvestmentConstants));
static_assert(sizeof(HashNameRecord)
              == hash_names::kNameLength + sizeof(std::uint32_t) + 4 * sizeof(std::uint8_t));
static_assert(sizeof(ScenarioRecord)
              == scenarios::kNameCapacity + sizeof(std::uint32_t) + 10 * sizeof(std::uint8_t)
                     + scenarios::kSpawnStemCapacity
                     + 2 * scenarios::kBubbleCapacity * sizeof(std::uint8_t)
                     + scenarios::kBubbleCapacity * sizeof(std::uint32_t)
                     + scenarios::kBubbleCapacity * sizeof(std::uint16_t)
                     + (scenarios::kDestinationGroupCapacity
                        + scenarios::kDestinationPackageCapacity)
                           * sizeof(std::uint16_t));
static_assert(sizeof(SpawnStemRecord)
              == spawn_sets::kStemNameCapacity + sizeof(std::uint32_t) + 3 * sizeof(std::uint16_t)
                     + 2 * sizeof(std::uint8_t));
static_assert(sizeof(SpawnNameHashRecord)
              == 2 * sizeof(std::uint32_t)
                     + (1 + spawn_sets::kPackageCapacity) * sizeof(std::uint16_t)
                     + 6 * sizeof(std::uint8_t) + spawn_sets::kBubbleMaskBytes);
static_assert(sizeof(RosterGroupRecord)
              == 2 * sizeof(std::uint32_t) + sizeof(std::uint16_t)
                     + 2 * scenarios::kRosterSlotCapacity * sizeof(std::uint8_t));
static_assert(sizeof(ProgressionRecord) == sizeof(std::uint16_t) + 2 * sizeof(std::uint8_t));
static_assert(sizeof(AbilityBucketRecord)
              == sizeof(std::uint16_t) + 6 * sizeof(std::uint8_t)
                     + 2 * abilities::kBucketCapacity * sizeof(std::uint8_t)
                     + (abilities::kBucketCapacity * abilities::kBucketHashCapacity
                        + abilities::kOverflowCapacity)
                           * sizeof(std::uint32_t));
static_assert(sizeof(NamedRecord)
              == content::kDefinitionNameCapacity + 2 * sizeof(std::uint16_t)
                     + 2 * sizeof(std::uint32_t));
static_assert(sizeof(ItemRecord)
              == sizeof(std::uint32_t) + sizeof(std::uint16_t) + 2 * sizeof(std::uint8_t));
static_assert(sizeof(ItemDetailRecord)
              == 4 * sizeof(std::uint16_t) + 8 * sizeof(std::uint8_t) + sizeof(std::int32_t)
                     + sizeof(std::uint32_t)
                     + 2 * items::details::kInitialPlugCapacity * sizeof(std::uint16_t)
                     + items::details::kStatCapacity * (sizeof(std::uint8_t) + sizeof(std::int32_t))
                     + items::details::kSandboxPerkCapacity * sizeof(std::uint16_t)
                     + items::details::kRenderOverrideCapacity
                           * (2 * sizeof(std::uint8_t) + sizeof(std::uint16_t)));
static_assert(sizeof(InventoryBucketRecord)
              == 2 * sizeof(std::uint8_t) + 2 * sizeof(std::uint16_t));
static_assert(sizeof(SocketEntryListRecord)
              == sizeof(std::uint32_t) + sizeof(std::uint16_t) + 2 * sizeof(std::uint8_t)
                     + sizeof(std::uint64_t));
static_assert(sizeof(SocketEntryTableRecord)
              == sizeof(std::uint16_t) + 2 * sizeof(std::uint8_t)
                     + socket_entry_lists::kEntryCapacity
                           * (sizeof(std::uint32_t) + 2 * sizeof(std::uint8_t)));

/**
 * Extends the cache payload checksum with one written record.
 * @tparam Value Trivially copied packed cache record.
 * @param checksum Current checksum state.
 * @param value Record bytes in their disk form.
 * @return Checksum after every record byte is mixed in.
 */
template <typename Value>
[[nodiscard]] std::uint64_t checksum_value(std::uint64_t checksum, const Value& value) noexcept {
    const auto bytes = std::as_bytes(std::span(&value, 1));
    for (const std::byte byte : bytes) {
        checksum ^= std::to_integer<std::uint8_t>(byte);
        checksum *= kChecksumPrime;
    }
    return checksum;
}

} // namespace sunrise::state::build_data::cache::records
