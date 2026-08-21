#include <Windows.h>

#include <cstdint>
#include <span>
#include <string_view>

#include "../../core/filesystem/path.h"
#include "../content/content_catalog.h"
#include "abilities/ability_bucket_catalog.h"
#include "cache/internal.h"
#include "collectibles/collectible_catalog.h"
#include "constants/investment_constant_catalog.h"
#include "hash_names/hash_name_catalog.h"
#include "inventory/buckets/inventory_bucket_catalog.h"
#include "items/details/item_detail_catalog.h"
#include "items/socket_plugs/socket_plug_catalog.h"
#include "material_requirements/material_requirement_catalog.h"
#include "progressions/progression_catalog.h"
#include "records/record_catalog.h"
#include "runtime.h"
#include "runtime/build_data_catalog_runtime.h"
#include "runtime/domain_markers.h"
#include "runtime/persistence/build_data_persistence.h"
#include "scenarios/scenario_catalog.h"
#include "socket_entry_lists/socket_entry_list_catalog.h"
#include "spawn_sets/spawn_set_catalog.h"
#include "vendors/vendor_catalog.h"

namespace sunrise::state::build_data {
namespace {

/** One cache directory holds the generated build data under the artifact root. */
constexpr std::wstring_view kCacheDirectorySuffix = L"\\cache";
/** One reusable file stores all extracted build mappings. */
constexpr std::wstring_view kCacheFileSuffix = L"\\cache\\build_data.bin";

} // namespace

/** Loads one full cache, or leaves every domain ready for first-boot extraction. */
bool initialize(void* module, std::uint64_t configuredEquipmentHash) noexcept {
    runtime::persistence::Context& persistenceState = runtime::persistence::context();
    AcquireSRWLockExclusive(&persistenceState.lock);
    runtime::persistence::clear_locked(persistenceState);
    runtime::clear_catalogs();
    if (module == nullptr) {
        ReleaseSRWLockExclusive(&persistenceState.lock);
        return true;
    }

    core::path::Buffer moduleDirectory;
    if (!core::path::artifact_directory(module, moduleDirectory)) {
        ReleaseSRWLockExclusive(&persistenceState.lock);
        return false;
    }
    persistenceState.cacheDirectory = moduleDirectory;
    persistenceState.cachePath = moduleDirectory;
    if (!core::path::append(persistenceState.cacheDirectory, kCacheDirectorySuffix)
        || !core::path::append(persistenceState.cachePath, kCacheFileSuffix)
        || !cache::current_build_identity(configuredEquipmentHash,
                                          persistenceState.buildIdentity)) {
        runtime::persistence::clear_locked(persistenceState);
        ReleaseSRWLockExclusive(&persistenceState.lock);
        return false;
    }
    persistenceState.enabled = true;

    cache::records::DomainCounts counts{};
    const cache::LoadStatus status =
        cache::load(persistenceState.cachePath.chars.data(),
                    persistenceState.buildIdentity,
                    runtime::persistence::scratch_domains(persistenceState),
                    counts);
    if (status == cache::LoadStatus::missing) {
        runtime::persistence::release_scratch_locked(persistenceState);
        ReleaseSRWLockExclusive(&persistenceState.lock);
        return true;
    }
    if (status == cache::LoadStatus::stale) {
        // A stale cache is replaced only after every domain is complete.
        persistenceState.replaceStaleCache = true;
        runtime::persistence::release_scratch_locked(persistenceState);
        ReleaseSRWLockExclusive(&persistenceState.lock);
        return true;
    }
    const cache::records::Domains domains =
        runtime::persistence::occupied_domains(persistenceState, counts);
    bool detailsReplaced = false;
    if (domains.itemDetails.empty()) {
        items::details::clear();
        detailsReplaced = true;
    } else {
        detailsReplaced = items::details::replace(domains.itemDetails);
    }
    const constants::InvestmentConstants cachedConstants{
        domains.constants.extracted != 0,
        domains.constants.lightStatRow,
        domains.constants.characterStatRows,
    };
    if (status != cache::LoadStatus::loaded || !constants::replace(cachedConstants)
        || !content::replace(domains.named) || !content::seal() || !items::replace(domains.items)
        || !collectibles::replace(domains.collectibles)
        || !material_requirements::replace(domains.materialRequirementSets)
        || !inventory::buckets::replace(domains.inventoryBuckets)
        || !socket_entry_lists::replace(domains.socketEntryLists)
        // The per-entry tables are what the subclass selection reads. Without them a cache hit
        // makes the lists ready, the package build skips itself, and no ability is picked.
        || !socket_entry_lists::replace_entry_tables(domains.socketEntryTables) || !detailsReplaced
        || !items::socket_plugs::replace(
            domains.socketPlugRules, domains.socketPlugPools, domains.socketPlugMembers)
        || !abilities::replace(domains.abilityBuckets)
        || !progressions::replace(domains.progressions)
        || !records::replace(domains.records)
        // The layouts are what activity message 1 reads. Without them a cache hit makes the
        // other domains ready, the package build skips itself, and every destination falls back.
        || !scenarios::replace(domains.scenarios, domains.rosterGroups)
        // An empty catalog is complete, so the spawn-set replace is skipped rather than failed.
        || (!domains.spawnStems.empty()
            && !spawn_sets::replace(
                domains.spawnStems, domains.spawnNameHashes, domains.spawnPoints))
        // An empty catalog is complete, so the vendor replace is skipped rather than failed.
        || (!domains.vendorIndex.empty()
            && !vendors::replace(domains.vendorIndex,
                                 domains.vendorDefinitions,
                                 domains.vendorSaleRows,
                                 domains.vendorInstalledRows))
        || !hash_names::replace(domains.hashNames)) {
        // No domain remains published when any catalog rejects the cache transaction.
        runtime::clear_catalogs();
        runtime::persistence::clear_locked(persistenceState);
        ReleaseSRWLockExclusive(&persistenceState.lock);
        return false;
    }
    runtime::details::publish();
    runtime::named::publish();
    runtime::ability_buckets::publish();
    runtime::spawn_catalog::publish();
    runtime::name_catalog::publish();
    persistenceState.persisted = true;
    runtime::persistence::release_scratch_locked(persistenceState);
    ReleaseSRWLockExclusive(&persistenceState.lock);
    return true;
}

/** Clears all generated build mappings and persistence paths. */
void shutdown() noexcept {
    runtime::persistence::Context& persistenceState = runtime::persistence::context();
    AcquireSRWLockExclusive(&persistenceState.lock);
    runtime::clear_catalogs();
    runtime::persistence::clear_locked(persistenceState);
    ReleaseSRWLockExclusive(&persistenceState.lock);
}

} // namespace sunrise::state::build_data
