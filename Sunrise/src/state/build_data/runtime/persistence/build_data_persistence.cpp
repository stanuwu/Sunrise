#include "build_data_persistence.h"

#include <Windows.h>

#include <algorithm>
#include <span>
#include <vector>

#include "../../../../core/ui/busy/busy.h"
#include "../../abilities/ability_bucket_catalog.h"
#include "../../cache/internal.h"
#include "../../cache/records/validation.h"
#include "../../collectibles/collectible_catalog.h"
#include "../../constants/investment_constant_catalog.h"
#include "../../hash_names/hash_name_catalog.h"
#include "../../inventory/buckets/inventory_bucket_catalog.h"
#include "../../items/details/item_detail_catalog.h"
#include "../../items/socket_plugs/socket_plug_catalog.h"
#include "../../material_requirements/material_requirement_catalog.h"
#include "../../progressions/progression_catalog.h"
#include "../../records/record_catalog.h"
#include "../../runtime.h"
#include "../../scenarios/scenario_catalog.h"
#include "../../socket_entry_lists/socket_entry_list_catalog.h"
#include "../../spawn_sets/spawn_set_catalog.h"
#include "../../vendors/vendor_catalog.h"
#include "../build_data_catalog_runtime.h"
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
           && inventory::buckets::snapshot(scratch.inventoryBuckets, counts.inventoryBuckets)
           && socket_entry_lists::snapshot(scratch.socketEntryLists, counts.socketEntryLists)
           && socket_entry_lists::snapshot_entry_tables(scratch.socketEntryTables,
                                                        counts.socketEntryTables)
           && abilities::snapshot(scratch.abilityBuckets, counts.abilityBuckets)
           && progressions::snapshot(scratch.progressions, counts.progressions)
           && records::snapshot(scratch.records, counts.records)
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
           && cache::records::canonicalize(scratch, counts);
}

} // namespace

/** @return The process-wide persistence context, shared by lifecycle and writer code. */
Context& context() noexcept {
    return g_context;
}

/** @return True when every extracted domain is complete in State. */
bool all_domains_ready() noexcept {
    constants::InvestmentConstants published{};
    return runtime::named::ready() && item_definitions_ready() && configured_item_details_ready()
           && collectible_definitions_ready() && socket_plug_rules_ready()
           && material_requirement_sets_ready() && inventory_bucket_descriptors_ready()
           && socket_entry_lists_ready() && ability_buckets_ready()
           && progression_definitions_ready() && record_definitions_ready()
           && scenario_layouts_ready() && spawn_sets_ready()
           && hash_names_ready() && constants::find(published);
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
    const auto recordRows = ensure_scratch<records::Definition, records::kDefinitionCapacity>(
        state.recordScratch);
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
        inventoryBuckets,
        socketEntryLists,
        socketEntryTables,
        abilityBuckets,
        progressions,
        recordRows,
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
    };
}

/**
 * Releases one transient cache snapshot bank without walking its capacity.
 * @param storage Bank emptied and handed back to the allocator.
 */
template <typename Value> void release_bank(std::vector<Value>& storage) noexcept {
    storage.clear();
    storage.shrink_to_fit();
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
    release_bank(state.inventoryBucketScratch);
    release_bank(state.socketEntryListScratch);
    release_bank(state.socketEntryTableScratch);
    release_bank(state.abilityBucketScratch);
    release_bank(state.progressionScratch);
    release_bank(state.recordScratch);
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
    state.constantsScratch = {};
}

/** Clears fixed cache paths, identity, flags, and snapshot storage. */
void clear_locked(Context& state) noexcept {
    release_scratch_locked(state);
    state.cacheDirectory = {};
    state.cachePath = {};
    state.buildIdentity = {};
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
    };
}

/** Saves one complete canonical snapshot when every domain is ready. */
bool persist_if_complete_locked(Context& state) noexcept {
    if (!all_domains_ready() || state.persisted || !state.enabled) {
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

/** @return True only when every domain is ready and any needed cache write works. */
bool persist() noexcept {
    runtime::persistence::Context& state = runtime::persistence::context();
    AcquireSRWLockExclusive(&state.lock);
    const bool result = runtime::persistence::all_domains_ready()
                        && runtime::persistence::persist_if_complete_locked(state);
    ReleaseSRWLockExclusive(&state.lock);
    return result;
}

} // namespace sunrise::state::build_data
