#include "build_data_persistence.h"

#include <Windows.h>

#include <span>
#include <vector>

#include "../../../../core/ui/busy/busy.h"
#include "../../abilities/ability_bucket_catalog.h"
#include "../../bounties/bounty_catalog.h"
#include "../../cache/internal.h"
#include "../../cache/records/validation.h"
#include "../../collectibles/collectible_catalog.h"
#include "../../constants/investment_constant_catalog.h"
#include "../../hash_names/hash_name_catalog.h"
#include "../../inventory/buckets/inventory_bucket_catalog.h"
#include "../../items/catalysts/exotic_catalyst_catalog.h"
#include "../../items/details/item_detail_catalog.h"
#include "../../items/socket_plugs/socket_plug_catalog.h"
#include "../../material_requirements/material_requirement_catalog.h"
#include "../../nodes/node_catalog.h"
#include "../../progressions/progression_catalog.h"
#include "../../records/record_catalog.h"
#include "../../rewards/reward_catalog.h"
#include "../../runtime.h"
#include "../../scenarios/scenario_catalog.h"
#include "../../season_pass/season_pass_catalog.h"
#include "../../sobjects/sobject_catalog.h"
#include "../../socket_entry_lists/socket_entry_list_catalog.h"
#include "../../spawn_sets/spawn_set_catalog.h"
#include "../../vendors/vendor_catalog.h"
#include "../domain_markers.h"

namespace sunrise::state::build_data::runtime::persistence {
namespace {

Context g_context;

/**
 * Lazily sizes one bounded cache-snapshot bank and exposes all rows.
 * @param storage Bank held by the caller's context, resized on first use.
 * @return The whole bank.
 */
template <typename Value, std::size_t Capacity>
[[nodiscard]] std::span<Value> ensure_scratch(std::vector<Value>& storage) noexcept {
    if (storage.size() != Capacity) {
        storage.assign(Capacity, Value{});
    }
    return storage;
}

/** @param value Published runtime constants. @return The packed header form. */
[[nodiscard]] cache::records::InvestmentConstants
to_record(const constants::InvestmentConstants& value) noexcept {
    return {value.lightStatRow,
            value.weaponPowerStatRow,
            value.characterStatRows,
            value.extracted ? std::uint8_t{1} : std::uint8_t{0}};
}

/**
 * Copies all generated catalogs into fixed scratch storage.
 * @param state Its lock must already be held exclusively.
 * @param counts Receives every copied row count.
 * @return True when every snapshot fits and the canonical sort works.
 */
[[nodiscard]] bool snapshot_domains_locked(Context& state,
                                           cache::records::DomainCounts& counts) noexcept {
    counts = {};
    const cache::records::MutableDomains scratch = scratch_domains(state);
    *scratch.constants = to_record(constants::snapshot());
    gameplay::entity_object_types::Fingerprint objectFingerprint{};
    return content::snapshot(scratch.named, counts.named)
           && items::snapshot(scratch.items, counts.items)
           && collectibles::snapshot(scratch.collectibles, counts.collectibles)
           && material_requirements::snapshot(scratch.materialRequirementSets,
                                              counts.materialRequirementSets)
           && items::details::snapshot(scratch.itemDetails, counts.itemDetails)
           && items::socket_plugs::snapshot(scratch.socketPlugRules,
                                            counts.socketPlugRules,
                                            scratch.socketPlugPools,
                                            counts.socketPlugPools,
                                            scratch.socketPlugMembers,
                                            counts.socketPlugMembers)
           && items::catalysts::snapshot(scratch.exoticCatalysts, counts.exoticCatalysts)
           && inventory::buckets::snapshot(scratch.inventoryBuckets, counts.inventoryBuckets)
           && socket_entry_lists::snapshot(scratch.socketEntryLists, counts.socketEntryLists)
           && socket_entry_lists::snapshot_entry_tables(scratch.socketEntryTables,
                                                        counts.socketEntryTables)
           && abilities::snapshot(scratch.abilityBuckets, counts.abilityBuckets)
           && progressions::snapshot(scratch.progressions, counts.progressions)
           && progressions::snapshot_steps(scratch.progressionSteps, counts.progressionSteps)
           && season_pass::snapshot(scratch.seasonPassRewards, counts.seasonPassRewards)
           && bounties::snapshot(scratch.bounties, counts.bounties)
           && rewards::snapshot(scratch.rewardPools, counts.rewardPools)
           && rewards::snapshot(scratch.rewardEntries, counts.rewardEntries)
           && rewards::snapshot(scratch.rewardItems, counts.rewardItems)
           && rewards::snapshot(scratch.rewardInstructions, counts.rewardInstructions)
           && rewards::snapshot(scratch.rewardModifiers, counts.rewardModifiers)
           && rewards::snapshot(scratch.rewardSockets, counts.rewardSockets)
           && records::snapshot(scratch.records, counts.records)
           && records::snapshot_objectives(scratch.recordObjectives, counts.recordObjectives)
           && records::snapshot_intervals(scratch.recordIntervals, counts.recordIntervals)
           && records::snapshot_rewards(scratch.recordRewards, counts.recordRewards)
           && nodes::snapshot(scratch.nodes, counts.nodes)
           && sobjects::snapshot(scratch.sobjects, counts.sobjects)
           && scenarios::snapshot(scratch.scenarios, counts.scenarios)
           && scenarios::snapshot_groups(scratch.rosterGroups, counts.rosterGroups)
           && spawn_sets::snapshot(scratch.spawnStems, counts.spawnStems)
           && spawn_sets::snapshot_hashes(scratch.spawnNameHashes, counts.spawnNameHashes)
           && spawn_sets::snapshot_points(scratch.spawnPoints, counts.spawnPoints)
           && hash_names::snapshot(scratch.hashNames, counts.hashNames)
           && vendors::snapshot_index(scratch.vendorIndex, counts.vendorIndex)
           && vendors::snapshot_definitions(scratch.vendorDefinitions, counts.vendorDefinitions)
           && vendors::snapshot_sale_rows(scratch.vendorSaleRows, counts.vendorSaleRows)
           && vendors::snapshot_installed_rows(scratch.vendorInstalledRows,
                                               counts.vendorInstalledRows)
           && gameplay::entity_position_profiles::snapshot(
               scratch.positionProfiles, counts.positionProfiles, *scratch.positionFingerprint)
           && gameplay::entity_object_types::snapshot(
               scratch.objectTypes, counts.objectTypes, objectFingerprint)
           && objectFingerprint == *scratch.positionFingerprint
           && cache::records::canonicalize(scratch, counts);
}

/** @return True when every required extracted domain is complete in State. */
[[nodiscard]] bool required_domains_ready() noexcept {
    constants::InvestmentConstants published{};
    return runtime::named::ready() && item_definitions_ready() && configured_item_details_ready()
           && collectible_definitions_ready() && socket_plug_rules_ready()
           && material_requirement_sets_ready() && inventory_bucket_descriptors_ready()
           && socket_entry_lists_ready() && ability_buckets_ready()
           && progression_definitions_ready() && season_pass_ready() && repeatable_bounties_ready()
           && reward_definitions_ready() && record_definitions_ready() && node_definitions_ready()
           && sobject_definitions_ready() && scenario_layouts_ready() && spawn_sets_ready()
           && hash_names_ready() && vendor_catalog_ready()
           && gameplay::entity_position_profiles::available()
           && gameplay::entity_object_types::available() && constants::find(published);
}

} // namespace

/** @return The process-wide persistence context, shared by lifecycle and writer code. */
Context& context() noexcept {
    return g_context;
}

/** Gives mutable views over every fixed snapshot buffer. */
cache::records::MutableDomains scratch_domains(Context& state) noexcept {
    const auto named = ensure_scratch<content::Definition, content::kDefinitionCatalogCapacity>(
        state.namedScratch);
    const auto items =
        ensure_scratch<build_data::items::Definition, build_data::items::kDefinitionCapacity>(
            state.itemScratch);
    const auto collectibles =
        ensure_scratch<build_data::collectibles::Definition,
                       build_data::collectibles::kDefinitionCapacity>(state.collectibleScratch);
    const auto materialRequirementSets = ensure_scratch<material_requirements::Definition,
                                                        material_requirements::kDefinitionCapacity>(
        state.materialRequirementSetScratch);
    const auto itemDetails =
        ensure_scratch<build_data::items::details::Definition,
                       build_data::items::details::kDefinitionCapacity>(state.itemDetailScratch);
    const auto socketPlugRules =
        ensure_scratch<build_data::items::socket_plugs::Rule,
                       build_data::items::socket_plugs::kRuleCapacity>(state.socketPlugRuleScratch);
    const auto socketPlugPools =
        ensure_scratch<build_data::items::socket_plugs::Pool,
                       build_data::items::socket_plugs::kPoolCapacity>(state.socketPlugPoolScratch);
    const auto socketPlugMembers = ensure_scratch<build_data::items::socket_plugs::Member,
                                                  build_data::items::socket_plugs::kMemberCapacity>(
        state.socketPlugMemberScratch);
    const auto exoticCatalysts = ensure_scratch<build_data::items::catalysts::Definition,
                                                build_data::items::catalysts::kDefinitionCapacity>(
        state.exoticCatalystScratch);
    const auto inventoryBuckets =
        ensure_scratch<inventory::buckets::Descriptor, inventory::buckets::kDescriptorCapacity>(
            state.inventoryBucketScratch);
    const auto socketEntryLists =
        ensure_scratch<socket_entry_lists::Definition, socket_entry_lists::kDefinitionCapacity>(
            state.socketEntryListScratch);
    const auto socketEntryTables =
        ensure_scratch<socket_entry_lists::EntryTable, socket_entry_lists::kEntryTableCapacity>(
            state.socketEntryTableScratch);
    const auto abilityBuckets =
        ensure_scratch<abilities::Definition, abilities::kDefinitionCapacity>(
            state.abilityBucketScratch);
    const auto progressions =
        ensure_scratch<progressions::Definition, progressions::kDefinitionCapacity>(
            state.progressionScratch);
    const auto recordRows =
        ensure_scratch<records::Definition, records::kDefinitionCapacity>(state.recordScratch);
    const auto recordObjectives = ensure_scratch<records::Objective, records::kObjectiveCapacity>(
        state.recordObjectiveScratch);
    const auto recordIntervals =
        ensure_scratch<records::Interval, records::kIntervalCapacity>(state.recordIntervalScratch);
    const auto recordRewards =
        ensure_scratch<records::Reward, records::kRewardCapacity>(state.recordRewardScratch);
    const auto nodeRows =
        ensure_scratch<nodes::Definition, nodes::kDefinitionCapacity>(state.nodeScratch);
    const auto sobjectRows =
        ensure_scratch<sobjects::Definition, sobjects::kDefinitionCapacity>(state.sobjectScratch);
    const auto scenarios = ensure_scratch<scenarios::Definition, scenarios::kDefinitionCapacity>(
        state.scenarioScratch);
    const auto rosterGroups =
        ensure_scratch<scenarios::RosterGroup, scenarios::kRosterGroupCapacity>(
            state.rosterGroupScratch);
    const auto spawnStems =
        ensure_scratch<spawn_sets::Stem, spawn_sets::kStemCapacity>(state.spawnStemScratch);
    const auto spawnNameHashes =
        ensure_scratch<spawn_sets::NameHash, spawn_sets::kNameHashCapacity>(
            state.spawnNameHashScratch);
    const auto spawnPoints =
        ensure_scratch<spawn_sets::Point, spawn_sets::kPointCapacity>(state.spawnPointScratch);
    const auto hashNames =
        ensure_scratch<hash_names::Name, hash_names::kNameCapacity>(state.hashNameScratch);
    const auto vendorIndex =
        ensure_scratch<vendors::IndexEntry, vendors::kIndexCapacity>(state.vendorIndexScratch);
    const auto vendorDefinitions =
        ensure_scratch<vendors::Definition, vendors::kDefinitionCapacity>(
            state.vendorDefinitionScratch);
    const auto vendorSaleRows =
        ensure_scratch<vendors::SaleRow, vendors::kSaleRowCapacity>(state.vendorSaleRowScratch);
    const auto vendorInstalledRows =
        ensure_scratch<vendors::InstalledRow, vendors::kInstalledRowCapacity>(
            state.vendorInstalledRowScratch);
    const auto progressionSteps = ensure_scratch<progressions::Step, progressions::kStepCapacity>(
        state.progressionStepScratch);
    const auto seasonPassRewards =
        ensure_scratch<season_pass::Reward, season_pass::kRewardCapacity>(
            state.seasonPassRewardScratch);
    const auto bountyRows =
        ensure_scratch<bounties::Definition, bounties::kDefinitionCapacity>(state.bountyScratch);
    const auto positionProfiles = ensure_scratch<gameplay::entity_position_profiles::Row,
                                                 gameplay::entity_position_profiles::kMaximumRows>(
        state.positionProfileScratch);
    return {
        &state.constantsScratch,
        named,
        items,
        collectibles,
        materialRequirementSets,
        itemDetails,
        socketPlugRules,
        socketPlugPools,
        socketPlugMembers,
        exoticCatalysts,
        inventoryBuckets,
        socketEntryLists,
        socketEntryTables,
        abilityBuckets,
        progressions,
        recordRows,
        nodeRows,
        sobjectRows,
        scenarios,
        rosterGroups,
        spawnStems,
        spawnNameHashes,
        spawnPoints,
        hashNames,
        vendorIndex,
        vendorDefinitions,
        vendorSaleRows,
        vendorInstalledRows,
        positionProfiles,
        &state.positionFingerprint,
        ensure_scratch<gameplay::entity_object_types::Row,
                       gameplay::entity_object_types::kMaximumRows>(state.objectTypeScratch),
        recordObjectives,
        recordIntervals,
        recordRewards,
        progressionSteps,
        seasonPassRewards,
        bountyRows,
        ensure_scratch<rewards::Pool, rewards::kPoolCapacity>(state.rewardPoolScratch),
        ensure_scratch<rewards::Entry, rewards::kEntryCapacity>(state.rewardEntryScratch),
        ensure_scratch<rewards::Item, rewards::kItemCapacity>(state.rewardItemScratch),
        ensure_scratch<rewards::Instruction, rewards::kInstructionCapacity>(
            state.rewardInstructionScratch),
        ensure_scratch<rewards::Modifier, rewards::kModifierCapacity>(state.rewardModifierScratch),
        ensure_scratch<rewards::SocketOverride, rewards::kSocketOverrideCapacity>(
            state.rewardSocketScratch),
    };
}

/**
 * Releases one transient cache snapshot bank without walking its capacity.
 * @param storage Bank emptied and handed back to the allocator.
 */
template <typename Value> static void release_bank(std::vector<Value>& storage) noexcept {
    std::vector<Value>{}.swap(storage);
}

/** Releases transient cache snapshot banks without allocating or walking their capacities. */
void release_scratch_locked(Context& state) noexcept {
    release_bank(state.namedScratch);
    release_bank(state.itemScratch);
    release_bank(state.collectibleScratch);
    release_bank(state.materialRequirementSetScratch);
    release_bank(state.itemDetailScratch);
    release_bank(state.socketPlugRuleScratch);
    release_bank(state.socketPlugPoolScratch);
    release_bank(state.socketPlugMemberScratch);
    release_bank(state.exoticCatalystScratch);
    release_bank(state.inventoryBucketScratch);
    release_bank(state.socketEntryListScratch);
    release_bank(state.socketEntryTableScratch);
    release_bank(state.abilityBucketScratch);
    release_bank(state.progressionScratch);
    release_bank(state.progressionStepScratch);
    release_bank(state.seasonPassRewardScratch);
    release_bank(state.bountyScratch);
    release_bank(state.rewardPoolScratch);
    release_bank(state.rewardEntryScratch);
    release_bank(state.rewardItemScratch);
    release_bank(state.rewardInstructionScratch);
    release_bank(state.rewardModifierScratch);
    release_bank(state.rewardSocketScratch);
    release_bank(state.recordScratch);
    release_bank(state.recordObjectiveScratch);
    release_bank(state.recordIntervalScratch);
    release_bank(state.recordRewardScratch);
    release_bank(state.nodeScratch);
    release_bank(state.sobjectScratch);
    release_bank(state.scenarioScratch);
    release_bank(state.rosterGroupScratch);
    release_bank(state.spawnStemScratch);
    release_bank(state.spawnNameHashScratch);
    release_bank(state.spawnPointScratch);
    release_bank(state.hashNameScratch);
    release_bank(state.vendorIndexScratch);
    release_bank(state.vendorDefinitionScratch);
    release_bank(state.vendorSaleRowScratch);
    release_bank(state.vendorInstalledRowScratch);
    release_bank(state.positionProfileScratch);
    release_bank(state.objectTypeScratch);
    state.positionFingerprint = {};
    state.constantsScratch = {};
}

/** Clears fixed cache paths, identity, flags, and snapshot storage. */
void clear_locked(Context& state) noexcept {
    release_scratch_locked(state);
    state.cacheDirectory = {};
    state.cachePath = {};
    state.buildIdentity = {};
    state.catalystError = items::catalysts::Error::none;
    state.enabled = false;
    state.persisted = false;
    state.replaceStaleCache = false;
}

/** Gives read-only views over the used rows of one canonical snapshot. */
cache::records::Domains occupied_domains(Context& state,
                                         const cache::records::DomainCounts& counts) noexcept {
    const std::span<const items::details::Definition> itemDetails{state.itemDetailScratch.data(),
                                                                  counts.itemDetails};
    const std::span<const items::socket_plugs::Rule> socketPlugRules{
        state.socketPlugRuleScratch.data(), counts.socketPlugRules};
    const std::span<const items::socket_plugs::Pool> socketPlugPools{
        state.socketPlugPoolScratch.data(), counts.socketPlugPools};
    const std::span<const items::socket_plugs::Member> socketPlugMembers{
        state.socketPlugMemberScratch.data(), counts.socketPlugMembers};
    const std::span<const items::catalysts::Definition> exoticCatalysts{
        state.exoticCatalystScratch.data(), counts.exoticCatalysts};
    return {
        state.constantsScratch,
        std::span<const content::Definition>{state.namedScratch.data(), counts.named},
        std::span<const build_data::items::Definition>{state.itemScratch.data(), counts.items},
        std::span<const build_data::collectibles::Definition>{state.collectibleScratch.data(),
                                                              counts.collectibles},
        std::span<const material_requirements::Definition>{
            state.materialRequirementSetScratch.data(), counts.materialRequirementSets},
        itemDetails,
        socketPlugRules,
        socketPlugPools,
        socketPlugMembers,
        exoticCatalysts,
        std::span<const inventory::buckets::Descriptor>{state.inventoryBucketScratch.data(),
                                                        counts.inventoryBuckets},
        std::span<const socket_entry_lists::Definition>{state.socketEntryListScratch.data(),
                                                        counts.socketEntryLists},
        std::span<const socket_entry_lists::EntryTable>{state.socketEntryTableScratch.data(),
                                                        counts.socketEntryTables},
        std::span<const abilities::Definition>{state.abilityBucketScratch.data(),
                                               counts.abilityBuckets},
        std::span<const progressions::Definition>{state.progressionScratch.data(),
                                                  counts.progressions},
        std::span<const records::Definition>{state.recordScratch.data(), counts.records},
        std::span<const nodes::Definition>{state.nodeScratch.data(), counts.nodes},
        std::span<const sobjects::Definition>{state.sobjectScratch.data(), counts.sobjects},
        std::span<const scenarios::Definition>{state.scenarioScratch.data(), counts.scenarios},
        std::span<const scenarios::RosterGroup>{state.rosterGroupScratch.data(),
                                                counts.rosterGroups},
        std::span<const spawn_sets::Stem>{state.spawnStemScratch.data(), counts.spawnStems},
        std::span<const spawn_sets::NameHash>{state.spawnNameHashScratch.data(),
                                              counts.spawnNameHashes},
        std::span<const spawn_sets::Point>{state.spawnPointScratch.data(), counts.spawnPoints},
        std::span<const hash_names::Name>{state.hashNameScratch.data(), counts.hashNames},
        std::span<const vendors::IndexEntry>{state.vendorIndexScratch.data(), counts.vendorIndex},
        std::span<const vendors::Definition>{state.vendorDefinitionScratch.data(),
                                             counts.vendorDefinitions},
        std::span<const vendors::SaleRow>{state.vendorSaleRowScratch.data(), counts.vendorSaleRows},
        std::span<const vendors::InstalledRow>{state.vendorInstalledRowScratch.data(),
                                               counts.vendorInstalledRows},
        std::span<const gameplay::entity_position_profiles::Row>{
            state.positionProfileScratch.data(), counts.positionProfiles},
        state.positionFingerprint,
        std::span<const gameplay::entity_object_types::Row>{state.objectTypeScratch.data(),
                                                            counts.objectTypes},
        std::span<const records::Objective>{state.recordObjectiveScratch.data(),
                                            counts.recordObjectives},
        std::span<const records::Interval>{state.recordIntervalScratch.data(),
                                           counts.recordIntervals},
        std::span<const records::Reward>{state.recordRewardScratch.data(), counts.recordRewards},
        std::span<const progressions::Step>{state.progressionStepScratch.data(),
                                            counts.progressionSteps},
        std::span<const season_pass::Reward>{state.seasonPassRewardScratch.data(),
                                             counts.seasonPassRewards},
        std::span<const bounties::Definition>{state.bountyScratch.data(), counts.bounties},
        std::span<const rewards::Pool>{state.rewardPoolScratch.data(), counts.rewardPools},
        std::span<const rewards::Entry>{state.rewardEntryScratch.data(), counts.rewardEntries},
        std::span<const rewards::Item>{state.rewardItemScratch.data(), counts.rewardItems},
        std::span<const rewards::Instruction>{state.rewardInstructionScratch.data(),
                                              counts.rewardInstructions},
        std::span<const rewards::Modifier>{state.rewardModifierScratch.data(),
                                           counts.rewardModifiers},
        std::span<const rewards::SocketOverride>{state.rewardSocketScratch.data(),
                                                 counts.rewardSockets},
    };
}

/** Saves one canonical snapshot when all domains required by its build are ready. */
bool persist_if_ready_locked(Context& state, bool catalystRequired) noexcept {
    if (!required_domains_ready() || (catalystRequired && !exotic_catalysts_ready())
        || state.persisted || !state.enabled) {
        return true;
    }
    cache::records::DomainCounts counts{};
    // One canonical snapshot keeps header counts and payload rows in the same commit.
    if (!snapshot_domains_locked(state, counts)) {
        return false;
    }
    // The write flushes every domain to disk once. It is the only stall here, so the overlay
    // covers just it.
    core::ui::busy::begin(core::ui::busy::Task::cacheWrite);
    const bool written =
        cache::write(state.cacheDirectory.chars.data(),
                     state.cachePath.chars.data(),
                     state.buildIdentity,
                     occupied_domains(state, counts),
                     state.replaceStaleCache ? cache::WriteDisposition::replaceStale
                                             : cache::WriteDisposition::createOnly);
    core::ui::busy::end(core::ui::busy::Task::cacheWrite);
    if (!written) {
        return false;
    }
    state.persisted = true;
    state.replaceStaleCache = false;
    release_scratch_locked(state);
    return true;
}

} // namespace sunrise::state::build_data::runtime::persistence

namespace sunrise::state::build_data {

/** Newly extracted package data replaces the shared cache only after all domains are ready. */
void invalidate_cache() noexcept {
    auto& state = runtime::persistence::context();
    AcquireSRWLockExclusive(&state.lock);
    state.persisted = false;
    state.replaceStaleCache = true;
    ReleaseSRWLockExclusive(&state.lock);
}

/** @return True when required domains are ready and any safe cache write works. */
bool persist() noexcept {
    runtime::persistence::Context& state = runtime::persistence::context();
    AcquireSRWLockExclusive(&state.lock);
    const bool requiredReady = runtime::persistence::required_domains_ready();
    bool result = false;
    switch (runtime::persistence::cache_action(
        requiredReady, exotic_catalysts_ready(), state.catalystError)) {
    case runtime::persistence::CacheAction::waitForDomains:
        break;
    case runtime::persistence::CacheAction::writeRequiredDomains:
        // Unsupported catalyst facts do not prevent the other build-bound domains from caching.
        result = runtime::persistence::persist_if_ready_locked(state, false);
        break;
    case runtime::persistence::CacheAction::writeCompleteCache:
        result = runtime::persistence::persist_if_ready_locked(state, true);
        break;
    }
    ReleaseSRWLockExclusive(&state.lock);
    return result;
}

} // namespace sunrise::state::build_data
