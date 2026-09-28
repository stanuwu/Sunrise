#pragma once

#include <cstddef>
#include <span>

#include "../../../content/content_catalog.h"
#include "../../../gameplay/external/entity_object_types.h"
#include "../../../gameplay/external/entity_position_profiles.h"
#include "../../abilities/definition.h"
#include "../../bounties/definition.h"
#include "../../collectibles/collectible_catalog.h"
#include "../../hash_names/definition.h"
#include "../../inventory/buckets/definition.h"
#include "../../items/catalysts/definition.h"
#include "../../items/details/definition.h"
#include "../../items/item_catalog.h"
#include "../../items/socket_plugs/definition.h"
#include "../../material_requirements/material_requirement_catalog.h"
#include "../../nodes/definition.h"
#include "../../progressions/definition.h"
#include "../../records/definition.h"
#include "../../rewards/definition.h"
#include "../../scenarios/definition.h"
#include "../../season_pass/definition.h"
#include "../../sobjects/sobject_catalog.h"
#include "../../socket_entry_lists/definition.h"
#include "../../spawn_sets/definition.h"
#include "../../vendors/definition.h"
#include "format.h"

namespace sunrise::state::build_data::cache::records {

/** Checked row counts for every generated cache domain. */
struct DomainCounts {
    std::size_t named{};
    std::size_t items{};
    std::size_t collectibles{};
    std::size_t materialRequirementSets{};
    std::size_t itemDetails{};
    std::size_t socketPlugRules{};
    std::size_t socketPlugPools{};
    std::size_t socketPlugMembers{};
    std::size_t exoticCatalysts{};
    std::size_t inventoryBuckets{};
    std::size_t socketEntryLists{};
    std::size_t socketEntryTables{};
    std::size_t abilityBuckets{};
    std::size_t progressions{};
    std::size_t records{};
    std::size_t nodes{};
    std::size_t sobjects{};
    std::size_t scenarios{};
    std::size_t rosterGroups{};
    std::size_t spawnStems{};
    std::size_t spawnNameHashes{};
    std::size_t spawnPoints{};
    std::size_t hashNames{};
    std::size_t vendorIndex{};
    std::size_t vendorDefinitions{};
    std::size_t vendorSaleRows{};
    std::size_t vendorInstalledRows{};
    std::size_t positionProfiles{};
    std::size_t objectTypes{};
    std::size_t recordObjectives{};
    std::size_t recordIntervals{};
    std::size_t recordRewards{};
    std::size_t progressionSteps{};
    std::size_t seasonPassRewards{};
    std::size_t bounties{};
    std::size_t rewardPools{};
    std::size_t rewardEntries{};
    std::size_t rewardItems{};
    std::size_t rewardInstructions{};
    std::size_t rewardModifiers{};
    std::size_t rewardSockets{};
};

/** Fixed caller storage used while decoding the cache domains. */
struct MutableDomains {
    /** Header scalars. The reader writes these straight through; no record array holds them. */
    InvestmentConstants* constants{};
    std::span<content::Definition> named;
    std::span<items::Definition> items;
    std::span<collectibles::Definition> collectibles;
    std::span<material_requirements::Definition> materialRequirementSets;
    std::span<items::details::Definition> itemDetails;
    std::span<items::socket_plugs::Rule> socketPlugRules;
    std::span<items::socket_plugs::Pool> socketPlugPools;
    std::span<items::socket_plugs::Member> socketPlugMembers;
    std::span<items::catalysts::Definition> exoticCatalysts;
    std::span<inventory::buckets::Descriptor> inventoryBuckets;
    std::span<socket_entry_lists::Definition> socketEntryLists;
    std::span<socket_entry_lists::EntryTable> socketEntryTables;
    std::span<abilities::Definition> abilityBuckets;
    std::span<progressions::Definition> progressions;
    std::span<build_data::records::Definition> records;
    std::span<nodes::Definition> nodes;
    std::span<sobjects::Definition> sobjects;
    std::span<scenarios::Definition> scenarios;
    std::span<scenarios::RosterGroup> rosterGroups;
    std::span<spawn_sets::Stem> spawnStems;
    std::span<spawn_sets::NameHash> spawnNameHashes;
    std::span<spawn_sets::Point> spawnPoints;
    std::span<hash_names::Name> hashNames;
    std::span<vendors::IndexEntry> vendorIndex;
    std::span<vendors::Definition> vendorDefinitions;
    std::span<vendors::SaleRow> vendorSaleRows;
    std::span<vendors::InstalledRow> vendorInstalledRows;
    std::span<gameplay::entity_position_profiles::Row> positionProfiles;
    gameplay::entity_position_profiles::Fingerprint* positionFingerprint{};
    std::span<gameplay::entity_object_types::Row> objectTypes;
    std::span<build_data::records::Objective> recordObjectives;
    std::span<build_data::records::Interval> recordIntervals;
    std::span<build_data::records::Reward> recordRewards;
    std::span<progressions::Step> progressionSteps;
    std::span<season_pass::Reward> seasonPassRewards;
    std::span<bounties::Definition> bounties;
    std::span<rewards::Pool> rewardPools;
    std::span<rewards::Entry> rewardEntries;
    std::span<rewards::Item> rewardItems;
    std::span<rewards::Instruction> rewardInstructions;
    std::span<rewards::Modifier> rewardModifiers;
    std::span<rewards::SocketOverride> rewardSockets;
};

/** Read-only complete views used for the checks and for cache encoding. */
struct Domains {
    InvestmentConstants constants{};
    std::span<const content::Definition> named;
    std::span<const items::Definition> items;
    std::span<const collectibles::Definition> collectibles;
    std::span<const material_requirements::Definition> materialRequirementSets;
    std::span<const items::details::Definition> itemDetails;
    std::span<const items::socket_plugs::Rule> socketPlugRules;
    std::span<const items::socket_plugs::Pool> socketPlugPools;
    std::span<const items::socket_plugs::Member> socketPlugMembers;
    std::span<const items::catalysts::Definition> exoticCatalysts;
    std::span<const inventory::buckets::Descriptor> inventoryBuckets;
    std::span<const socket_entry_lists::Definition> socketEntryLists;
    std::span<const socket_entry_lists::EntryTable> socketEntryTables;
    std::span<const abilities::Definition> abilityBuckets;
    std::span<const progressions::Definition> progressions;
    std::span<const build_data::records::Definition> records;
    std::span<const nodes::Definition> nodes;
    std::span<const sobjects::Definition> sobjects;
    std::span<const scenarios::Definition> scenarios;
    std::span<const scenarios::RosterGroup> rosterGroups;
    std::span<const spawn_sets::Stem> spawnStems;
    std::span<const spawn_sets::NameHash> spawnNameHashes;
    std::span<const spawn_sets::Point> spawnPoints;
    std::span<const hash_names::Name> hashNames;
    std::span<const vendors::IndexEntry> vendorIndex;
    std::span<const vendors::Definition> vendorDefinitions;
    std::span<const vendors::SaleRow> vendorSaleRows;
    std::span<const vendors::InstalledRow> vendorInstalledRows;
    std::span<const gameplay::entity_position_profiles::Row> positionProfiles;
    gameplay::entity_position_profiles::Fingerprint positionFingerprint{};
    std::span<const gameplay::entity_object_types::Row> objectTypes;
    std::span<const build_data::records::Objective> recordObjectives;
    std::span<const build_data::records::Interval> recordIntervals;
    std::span<const build_data::records::Reward> recordRewards;
    std::span<const progressions::Step> progressionSteps;
    std::span<const season_pass::Reward> seasonPassRewards;
    std::span<const bounties::Definition> bounties;
    std::span<const rewards::Pool> rewardPools;
    std::span<const rewards::Entry> rewardEntries;
    std::span<const rewards::Item> rewardItems;
    std::span<const rewards::Instruction> rewardInstructions;
    std::span<const rewards::Modifier> rewardModifiers;
    std::span<const rewards::SocketOverride> rewardSockets;
};

} // namespace sunrise::state::build_data::cache::records
