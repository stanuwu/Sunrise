#include "build_data_persistence.h"

#include <Windows.h>

#include <algorithm>
#include <span>

#include "../../../../core/ui/busy/busy.h"
#include "../../abilities/ability_bucket_catalog.h"
#include "../../cache/internal.h"
#include "../../cache/records/validation.h"
#include "../../constants/investment_constant_catalog.h"
#include "../../hash_names/hash_name_catalog.h"
#include "../../inventory/buckets/inventory_bucket_catalog.h"
#include "../../items/details/item_detail_catalog.h"
#include "../../progressions/progression_catalog.h"
#include "../../runtime.h"
#include "../../scenarios/scenario_catalog.h"
#include "../../socket_entry_lists/socket_entry_list_catalog.h"
#include "../../spawn_sets/spawn_set_catalog.h"
#include "../build_data_catalog_runtime.h"
#include "../domain_markers.h"

namespace sunrise::state::build_data::runtime::persistence {
namespace {

Context g_context;

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
           && items::details::snapshot(scratch.itemDetails, counts.itemDetails)
           && inventory::buckets::snapshot(scratch.inventoryBuckets, counts.inventoryBuckets)
           && socket_entry_lists::snapshot(scratch.socketEntryLists, counts.socketEntryLists)
           && socket_entry_lists::snapshot_entry_tables(scratch.socketEntryTables,
                                                        counts.socketEntryTables)
           && abilities::snapshot(scratch.abilityBuckets, counts.abilityBuckets)
           && progressions::snapshot(scratch.progressions, counts.progressions)
           && scenarios::snapshot(scratch.scenarios, counts.scenarios)
           && scenarios::snapshot_groups(scratch.rosterGroups, counts.rosterGroups)
           && spawn_sets::snapshot(scratch.spawnStems, counts.spawnStems)
           && spawn_sets::snapshot_hashes(scratch.spawnNameHashes, counts.spawnNameHashes)
           && hash_names::snapshot(scratch.hashNames, counts.hashNames)
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
           && inventory_bucket_descriptors_ready() && socket_entry_lists_ready()
           && ability_buckets_ready() && progression_definitions_ready() && scenario_layouts_ready()
           && spawn_sets_ready() && hash_names_ready() && constants::find(published);
}

/** Gives mutable views over every fixed snapshot buffer. */
cache::records::MutableDomains scratch_domains(Context& state) noexcept {
    return {
        &state.constantsScratch,
        state.namedScratch,
        state.itemScratch,
        state.itemDetailScratch,
        state.inventoryBucketScratch,
        state.socketEntryListScratch,
        state.socketEntryTableScratch,
        state.abilityBucketScratch,
        state.progressionScratch,
        state.scenarioScratch,
        state.rosterGroupScratch,
        state.spawnStemScratch,
        state.spawnNameHashScratch,
        state.hashNameScratch,
    };
}

/** Clears fixed cache paths, identity, flags, and snapshot storage. */
void clear_locked(Context& state) noexcept {
    const cache::records::MutableDomains scratch = scratch_domains(state);
    std::fill(scratch.named.begin(), scratch.named.end(), content::Definition{});
    std::fill(scratch.items.begin(), scratch.items.end(), items::Definition{});
    std::fill(scratch.itemDetails.begin(), scratch.itemDetails.end(), items::details::Definition{});
    std::fill(scratch.inventoryBuckets.begin(),
              scratch.inventoryBuckets.end(),
              inventory::buckets::Descriptor{});
    std::fill(scratch.socketEntryLists.begin(),
              scratch.socketEntryLists.end(),
              socket_entry_lists::Definition{});
    std::fill(scratch.socketEntryTables.begin(),
              scratch.socketEntryTables.end(),
              socket_entry_lists::EntryTable{});
    std::fill(
        scratch.abilityBuckets.begin(), scratch.abilityBuckets.end(), abilities::Definition{});
    std::fill(scratch.progressions.begin(), scratch.progressions.end(), progressions::Definition{});
    std::fill(scratch.scenarios.begin(), scratch.scenarios.end(), scenarios::Definition{});
    std::fill(scratch.rosterGroups.begin(), scratch.rosterGroups.end(), scenarios::RosterGroup{});
    std::fill(scratch.spawnStems.begin(), scratch.spawnStems.end(), spawn_sets::Stem{});
    std::fill(
        scratch.spawnNameHashes.begin(), scratch.spawnNameHashes.end(), spawn_sets::NameHash{});
    std::fill(scratch.hashNames.begin(), scratch.hashNames.end(), hash_names::Name{});
    state.constantsScratch = {};
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
    return {
        state.constantsScratch,
        std::span(state.namedScratch).first(counts.named),
        std::span(state.itemScratch).first(counts.items),
        std::span(state.itemDetailScratch).first(counts.itemDetails),
        std::span(state.inventoryBucketScratch).first(counts.inventoryBuckets),
        std::span(state.socketEntryListScratch).first(counts.socketEntryLists),
        std::span(state.socketEntryTableScratch).first(counts.socketEntryTables),
        std::span(state.abilityBucketScratch).first(counts.abilityBuckets),
        std::span(state.progressionScratch).first(counts.progressions),
        std::span(state.scenarioScratch).first(counts.scenarios),
        std::span(state.rosterGroupScratch).first(counts.rosterGroups),
        std::span(state.spawnStemScratch).first(counts.spawnStems),
        std::span(state.spawnNameHashScratch).first(counts.spawnNameHashes),
        std::span(state.hashNameScratch).first(counts.hashNames),
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
