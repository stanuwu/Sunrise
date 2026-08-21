#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../content/content_catalog.h"
#include "../../abilities/definition.h"
#include "../../collectibles/collectible_catalog.h"
#include "../../constants/definition.h"
#include "../../definition.h"
#include "../../hash_names/definition.h"
#include "../../items/catalysts/definition.h"
#include "../../items/details/definition.h"
#include "../../items/item_catalog.h"
#include "../../items/socket_plugs/definition.h"
#include "../../material_requirements/material_requirement_catalog.h"
#include "../../progressions/definition.h"
#include "../../scenarios/definition.h"
#include "../../spawn_sets/definition.h"
#include "../../vendors/definition.h"

namespace sunrise::state::build_data::cache::records {

/** These 8 ASCII bytes mark a Sunrise build-data file. */
inline constexpr std::array<char, 8> kCacheMagic{'S', 'U', 'N', 'R', 'I', 'S', 'E', 'B'};
/**
 * Current build-data cache format. An older cache is rebuilt rather than read.
 * Bump it when a stored shape changes, and when the extraction filling it changes what it writes.
 * A cached row survives a code change, so a corrected walk keeps publishing the old rows.
 * Development builds wrote formats 46 through 49, so those numbers cannot be reused.
 */
inline constexpr std::uint32_t kCacheFormatVersion = 51;
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
    std::uint32_t collectibleCount{};
    std::uint32_t materialRequirementSetCount{};
    std::uint32_t itemDetailCount{};
    std::uint32_t socketPlugRuleCount{};
    std::uint32_t socketPlugPoolCount{};
    std::uint32_t socketPlugMemberCount{};
    std::uint32_t exoticCatalystCount{};
    std::uint32_t inventoryBucketCount{};
    std::uint32_t socketEntryListCount{};
    std::uint32_t socketEntryTableCount{};
    std::uint32_t abilityBucketCount{};
    std::uint32_t progressionCount{};
    std::uint32_t scenarioCount{};
    std::uint32_t rosterGroupCount{};
    std::uint32_t spawnStemCount{};
    std::uint32_t spawnNameHashCount{};
    std::uint32_t spawnPointCount{};
    std::uint32_t hashNameCount{};
    std::uint32_t vendorIndexCount{};
    std::uint32_t vendorDefinitionCount{};
    std::uint32_t vendorSaleRowCount{};
    std::uint32_t vendorInstalledRowCount{};
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
    /** Native rarity ladder byte; 0 outside the ladder. */
    std::uint8_t tier{};
    std::uint16_t insertionMaterialRequirementSetIndex{
        items::kUnavailableMaterialRequirementSetIndex};
    std::uint16_t enabledMaterialRequirementSetIndex{
        items::kUnavailableMaterialRequirementSetIndex};
    std::uint32_t plugCategoryHash{};
    std::uint16_t rollSetIndex{};
    std::uint16_t linkedPlugIndex{items::kUnavailableLinkedPlugIndex};
};

/** Disk form of one material charged by a native Collections acquisition. */
struct MaterialRequirementRecord {
    std::uint32_t quantity{};
    std::uint16_t itemDefinitionIndex{collectibles::kUnavailableItemDefinitionIndex};
    std::uint16_t condition{material_requirements::kUnconditionalRequirement};
    std::uint8_t deleteOnAction{};
    std::uint8_t omitFromRequirements{};
};

/** Disk form of one native collectible ordinal, item link, and installed acquisition cost. */
struct CollectibleRecord {
    std::uint32_t collectibleHash{};
    std::uint32_t materialRequirementSetHash{};
    std::uint16_t collectibleIndex{};
    std::uint16_t itemDefinitionIndex{collectibles::kUnavailableItemDefinitionIndex};
    std::uint16_t materialRequirementSetIndex{
        collectibles::kUnavailableMaterialRequirementSetIndex};
    std::uint8_t materialRequirementCount{};
    /** Must be zero, so unused bytes have one canonical representation. */
    std::uint8_t reserved{};
    std::array<MaterialRequirementRecord, collectibles::kMaterialRequirementCapacity>
        materialRequirements{};
};

/** Disk form of one dense installed action-cost set. */
struct MaterialRequirementSetRecord {
    std::uint32_t requirementSetHash{};
    std::uint16_t requirementSetIndex{material_requirements::kUnavailableSetIndex};
    std::uint8_t requirementCount{};
    /** Must be zero, so unused bytes have one canonical representation. */
    std::uint8_t reserved{};
    std::array<MaterialRequirementRecord, material_requirements::kRequirementCapacity>
        requirements{};
};

/** Disk form of the supported item fields instance generation uses. */
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
    std::array<std::uint16_t, items::details::kArtClassCapacity> artArrangementIndices{};
    std::uint8_t sandboxPerkCount{};
    std::array<std::uint16_t, items::details::kSandboxPerkCapacity> sandboxPerks{};
    /** Material override rows in the stage order the character record folds them in. */
    std::uint8_t renderOverrideCount{};
    std::array<std::uint8_t, items::details::kRenderOverrideCapacity> overrideStages{};
    std::array<std::int8_t, items::details::kRenderOverrideCapacity> overrideKeys{};
    std::array<std::uint16_t, items::details::kRenderOverrideCapacity> overrideValues{};
};

/** Disk form of one exact item/lane-to-deduplicated-pool rule. */
struct SocketPlugRuleRecord {
    std::uint16_t itemDefinitionIndex{};
    std::uint8_t lane{};
    /** Must be zero so all unused bytes have one canonical value. */
    std::uint8_t reserved{};
    std::uint32_t poolIndex{};
};

/** Disk form of one contiguous range in the flat allowed-plug member bank. */
struct SocketPlugPoolRecord {
    std::uint32_t memberOffset{};
    std::uint32_t memberCount{};
};

/** Disk form of one native item-definition index allowed as a plug. */
struct SocketPlugMemberRecord {
    std::uint16_t itemDefinitionIndex{};
};

/** Disk form of one build-derived exotic weapon catalyst relation. */
struct ExoticCatalystRecord {
    std::uint32_t itemDefinitionHash{};
    std::uint16_t itemDefinitionIndex{};
    std::uint16_t completedPlugDefinitionIndex{};
    std::uint16_t progressPlugDefinitionIndex{};
    std::uint16_t effectDefinitionIndex{};
    std::uint16_t acquisitionDefinitionIndex{};
    std::array<std::uint16_t, items::catalysts::kCompletionFlagCapacity>
        completionFlagDefinitionIndices{};
    std::array<std::uint16_t, items::catalysts::kCompletionValueCapacity>
        completionValueIndices{};
    std::uint16_t objectiveDefinitionIndex{items::catalysts::kUnavailableObjectiveIndex};
    std::uint8_t socketLane{};
    std::uint8_t availability{};
    std::uint8_t completionFlagCount{};
    std::uint8_t completionValueCount{};
    std::array<std::int32_t, items::catalysts::kCompletionValueCapacity> completionValues{};
    std::int32_t objectiveValue{};
};

/** Disk form of one inventory-bucket array-routing descriptor. */
struct InventoryBucketRecord {
    std::uint8_t bucketId{};
    std::uint8_t arraySelector{};
    std::uint16_t firstSlot{};
    std::uint16_t slotCount{};
    std::int8_t equipmentSlot{inventory::buckets::kUnavailableEquipmentSlot};
    std::uint8_t reserved{};
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
    /** Groups published through the delta's per-bubble sub-blocks. */
    std::uint8_t bubbleGroupCount{};
    /** Must be zero, so the packed destination row always matches. */
    std::array<std::uint8_t, 2> reserved{};
    std::array<char, scenarios::kSpawnStemCapacity> spawnStem{};
    std::array<std::uint8_t, scenarios::kBubbleCapacity> bubbleStates{};
    /** Each bubble's own name hash, in the same order as the states. */
    std::array<std::uint32_t, scenarios::kBubbleCapacity> bubbleHashes{};
    /** Slice-set states each bubble declares, in the same order as the states. */
    std::array<std::uint8_t, scenarios::kBubbleCapacity> bubbleStateCounts{};
    /** Roster table indices, in publish order. */
    std::array<std::uint16_t, scenarios::kDestinationGroupCapacity> rosterGroups{};
    /** Roster table indices published per bubble, in publish order. */
    std::array<std::uint16_t, scenarios::kDestinationBubbleGroupCapacity> bubbleGroups{};
    /**
     * Bubbles each per-bubble group is published in, one bit per client bubble index.
     * Stored as bytes, low bubble first, so the row keeps its one-byte alignment.
     */
    std::array<std::array<std::uint8_t, scenarios::kBubbleMaskBytes>,
               scenarios::kDestinationBubbleGroupCapacity>
        bubbleGroupMasks{};
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

/** Disk form of one spawn point and the set it belongs to. */
struct SpawnPointRecord {
    std::array<float, spawn_sets::kPositionComponents> position{};
    std::uint32_t nameHash{};
    std::uint16_t stemIndex{};
    /** Must be zero, so the packed point row always matches. */
    std::array<std::uint8_t, 2> reserved{};
};

/** Disk form of one vendor index row. */
struct VendorIndexRecord {
    std::uint32_t definitionHash{};
    std::uint32_t definitionTag{};
    std::uint16_t index{};
    /** Must be zero, so the packed vendor index row always matches. */
    std::uint16_t reserved{};
};

/** Disk form of one extracted vendor definition and its flat-bank ranges. */
struct VendorDefinitionRecord {
    std::uint32_t definitionHash{};
    std::uint32_t definitionTag{};
    std::uint32_t definitionClass{};
    std::uint32_t definitionSize{};
    std::uint32_t installedRowBase{};
    std::uint32_t installedRowClass{};
    std::uint32_t saleRowBase{};
    std::uint32_t saleRowClass{};
    std::uint32_t thirdRowBase{};
    std::uint32_t thirdRowClass{};
    std::uint32_t saleRowOffset{};
    std::uint32_t installedRowOffset{};
    std::uint32_t resetIntervalRaw{};
    std::uint32_t resetPhaseRaw{};
    std::uint16_t index{};
    std::uint16_t installedCount{};
    std::uint16_t saleCount{};
    std::uint16_t thirdCount{};
};

/** Disk form of one vendor sale row. Unnamed fields keep their raw value. */
struct VendorSaleRowRecord {
    std::uint16_t vendorIndex{};
    std::uint16_t rowIndex{};
    std::uint16_t itemIndex{};
    std::uint16_t secondaryItemIndex{};
    std::int32_t installedIndex{};
    std::uint32_t raw104{};
    std::uint32_t raw108{};
    std::int32_t raw172{};
    std::uint32_t expressionCount8{};
    std::uint32_t nestedRecordCount{};
    std::uint32_t expressionCount120{};
    std::uint32_t count136{};
    std::uint32_t expressionCount160{};
    std::uint8_t featureBranch{};
    /** Must be zero, so the packed sale row always matches. */
    std::array<std::uint8_t, 3> reserved{};
};

/** Disk form of one vendor installed row, kept whole. */
struct VendorInstalledRowRecord {
    std::uint16_t vendorIndex{};
    std::uint16_t rowIndex{};
    std::array<std::uint8_t, vendors::kInstalledRowStride> raw{};
};

/** Disk form of one roster group object and its slots. */
struct RosterGroupRecord {
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint16_t slotCount{};
    std::array<std::uint8_t, scenarios::kRosterSlotCapacity> slotTypes{};
    std::array<std::uint8_t, scenarios::kRosterSlotCapacity> slotFlags{};
    /** Each slot's own index, from its descriptor. */
    std::array<std::uint16_t, scenarios::kRosterSlotCapacity> slotIndices{};
};

#pragma pack(pop)

static_assert(sizeof(Prefix) == kCacheMagic.size() + sizeof(std::uint32_t));
static_assert(sizeof(InvestmentConstants)
              == constants::kCharacterStatRowCount + 2 * sizeof(std::uint8_t));
static_assert(sizeof(Header)
              == kCacheMagic.size() + 27 * sizeof(std::uint32_t) + 2 * sizeof(std::uint64_t)
                     + sizeof(InvestmentConstants));
static_assert(sizeof(SpawnPointRecord)
              == spawn_sets::kPositionComponents * sizeof(float) + sizeof(std::uint32_t)
                     + sizeof(std::uint16_t) + 2 * sizeof(std::uint8_t));
static_assert(sizeof(VendorIndexRecord) == 2 * sizeof(std::uint32_t) + 2 * sizeof(std::uint16_t));
static_assert(sizeof(VendorDefinitionRecord)
              == 14 * sizeof(std::uint32_t) + 4 * sizeof(std::uint16_t));
static_assert(sizeof(VendorSaleRowRecord)
              == 4 * sizeof(std::uint16_t) + 9 * sizeof(std::uint32_t) + 4 * sizeof(std::uint8_t));
static_assert(sizeof(VendorInstalledRowRecord)
              == 2 * sizeof(std::uint16_t) + vendors::kInstalledRowStride);
static_assert(sizeof(HashNameRecord)
              == hash_names::kNameLength + sizeof(std::uint32_t) + 4 * sizeof(std::uint8_t));
static_assert(sizeof(ScenarioRecord)
              == scenarios::kNameCapacity + sizeof(std::uint32_t) + 10 * sizeof(std::uint8_t)
                     + scenarios::kSpawnStemCapacity
                     + 2 * scenarios::kBubbleCapacity * sizeof(std::uint8_t)
                     + scenarios::kBubbleCapacity * sizeof(std::uint32_t)
                     + scenarios::kBubbleCapacity * sizeof(std::uint16_t)
                     + (scenarios::kDestinationGroupCapacity
                        + scenarios::kDestinationPackageCapacity
                        + scenarios::kDestinationBubbleGroupCapacity)
                           * sizeof(std::uint16_t)
                     + scenarios::kDestinationBubbleGroupCapacity * scenarios::kBubbleMaskBytes);
static_assert(sizeof(SpawnStemRecord)
              == spawn_sets::kStemNameCapacity + sizeof(std::uint32_t) + 3 * sizeof(std::uint16_t)
                     + 2 * sizeof(std::uint8_t));
static_assert(sizeof(SpawnNameHashRecord)
              == 2 * sizeof(std::uint32_t)
                     + (1 + spawn_sets::kPackageCapacity) * sizeof(std::uint16_t)
                     + 6 * sizeof(std::uint8_t) + spawn_sets::kBubbleMaskBytes);
static_assert(sizeof(RosterGroupRecord)
              == 2 * sizeof(std::uint32_t) + sizeof(std::uint16_t)
                     + 2 * scenarios::kRosterSlotCapacity * sizeof(std::uint8_t)
                     + scenarios::kRosterSlotCapacity * sizeof(std::uint16_t));
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
              == 2 * sizeof(std::uint32_t) + 5 * sizeof(std::uint16_t) + 2 * sizeof(std::uint8_t));
static_assert(sizeof(MaterialRequirementRecord)
              == sizeof(std::uint32_t) + 2 * sizeof(std::uint16_t) + 2 * sizeof(std::uint8_t));
static_assert(sizeof(CollectibleRecord)
              == 2 * sizeof(std::uint32_t) + 3 * sizeof(std::uint16_t) + 2 * sizeof(std::uint8_t)
                     + collectibles::kMaterialRequirementCapacity
                           * sizeof(MaterialRequirementRecord));
static_assert(sizeof(MaterialRequirementSetRecord)
              == sizeof(std::uint32_t) + sizeof(std::uint16_t) + 2 * sizeof(std::uint8_t)
                     + material_requirements::kRequirementCapacity
                           * sizeof(MaterialRequirementRecord));
static_assert(sizeof(ItemDetailRecord)
              == 7 * sizeof(std::uint16_t) + 8 * sizeof(std::uint8_t) + sizeof(std::int32_t)
                     + sizeof(std::uint32_t)
                     + 2 * items::details::kInitialPlugCapacity * sizeof(std::uint16_t)
                     + items::details::kStatCapacity * (sizeof(std::uint8_t) + sizeof(std::int32_t))
                     + items::details::kSandboxPerkCapacity * sizeof(std::uint16_t)
                     + items::details::kRenderOverrideCapacity
                           * (2 * sizeof(std::uint8_t) + sizeof(std::uint16_t)));
static_assert(sizeof(SocketPlugRuleRecord)
              == sizeof(std::uint16_t) + 2 * sizeof(std::uint8_t) + sizeof(std::uint32_t));
static_assert(sizeof(SocketPlugPoolRecord) == 2 * sizeof(std::uint32_t));
static_assert(sizeof(SocketPlugMemberRecord) == sizeof(std::uint16_t));
static_assert(sizeof(ExoticCatalystRecord)
              == 6 * sizeof(std::uint32_t) + 14 * sizeof(std::uint16_t)
                     + 4 * sizeof(std::uint8_t));
static_assert(sizeof(InventoryBucketRecord)
              == 4 * sizeof(std::uint8_t) + 2 * sizeof(std::uint16_t));
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
