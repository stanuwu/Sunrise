#pragma once

#include <Windows.h>

#include <array>
#include <vector>

#include "../../../../core/filesystem/path.h"
#include "../../../content/content_catalog.h"
#include "../../abilities/definition.h"
#include "../../cache/records/domains.h"
#include "../../collectibles/collectible_catalog.h"
#include "../../constants/definition.h"
#include "../../definition.h"
#include "../../hash_names/definition.h"
#include "../../inventory/buckets/definition.h"
#include "../../items/catalysts/definition.h"
#include "../../items/details/definition.h"
#include "../../items/item_catalog.h"
#include "../../items/socket_plugs/definition.h"
#include "../../material_requirements/material_requirement_catalog.h"
#include "../../progressions/definition.h"
#include "../../scenarios/definition.h"
#include "../../socket_entry_lists/definition.h"
#include "../../spawn_sets/definition.h"
#include "../../vendors/definition.h"

namespace sunrise::state::build_data::runtime::persistence {

/** Cache action after one extraction pass. */
enum class CacheAction {
    waitForDomains,
    writeRequiredDomains,
    writeCompleteCache,
};

/**
 * Selects the only safe persistence action for the current extraction state.
 * @param requiredReady True when all required non-catalyst domains are ready.
 * @param catalystReady True when the complete catalyst catalog is published.
 * @param catalystError Exact result of the last catalyst derivation.
 * @return Wait, write required domains, or write every domain.
 */
[[nodiscard]] constexpr CacheAction cache_action(bool requiredReady,
                                                 bool catalystReady,
                                                 items::catalysts::Error catalystError) noexcept {
    if (!requiredReady) {
        return CacheAction::waitForDomains;
    }
    if (catalystReady) {
        return CacheAction::writeCompleteCache;
    }
    return catalystError == items::catalysts::Error::unsupportedBuild
               ? CacheAction::writeRequiredDomains
               : CacheAction::waitForDomains;
}

/** Fixed cache paths, identity, and canonical snapshot storage guarded by one State lock. */
struct Context {
    SRWLOCK lock{SRWLOCK_INIT};
    std::vector<content::Definition> namedScratch{};
    std::vector<items::Definition> itemScratch{};
    std::vector<collectibles::Definition> collectibleScratch{};
    std::vector<material_requirements::Definition> materialRequirementSetScratch{};
    std::vector<items::details::Definition> itemDetailScratch{};
    std::vector<items::socket_plugs::Rule> socketPlugRuleScratch{};
    std::vector<items::socket_plugs::Pool> socketPlugPoolScratch{};
    std::vector<items::socket_plugs::Member> socketPlugMemberScratch{};
    std::vector<items::catalysts::Definition> exoticCatalystScratch{};
    std::vector<inventory::buckets::Descriptor> inventoryBucketScratch{};
    std::vector<socket_entry_lists::Definition> socketEntryListScratch{};
    std::vector<socket_entry_lists::EntryTable> socketEntryTableScratch{};
    std::vector<abilities::Definition> abilityBucketScratch{};
    std::vector<progressions::Definition> progressionScratch{};
    std::vector<scenarios::Definition> scenarioScratch{};
    std::vector<scenarios::RosterGroup> rosterGroupScratch{};
    std::vector<spawn_sets::Stem> spawnStemScratch{};
    std::vector<spawn_sets::NameHash> spawnNameHashScratch{};
    std::vector<spawn_sets::Point> spawnPointScratch{};
    std::vector<hash_names::Name> hashNameScratch{};
    std::vector<vendors::IndexEntry> vendorIndexScratch{};
    std::vector<vendors::Definition> vendorDefinitionScratch{};
    std::vector<vendors::SaleRow> vendorSaleRowScratch{};
    std::vector<vendors::InstalledRow> vendorInstalledRowScratch{};
    cache::records::InvestmentConstants constantsScratch{};
    core::path::Buffer cacheDirectory;
    core::path::Buffer cachePath;
    BuildIdentity buildIdentity{};
    /** Last catalyst derivation error for this build. */
    items::catalysts::Error catalystError{items::catalysts::Error::none};
    bool enabled{};
    bool persisted{};
    bool replaceStaleCache{};
};

/** @return The process-wide persistence context, shared by lifecycle and writer code. */
[[nodiscard]] Context& context() noexcept;

/**
 * Clears fixed cache paths, identity, flags, and snapshot storage.
 * @param state Its lock must already be held exclusively.
 */
void clear_locked(Context& state) noexcept;

/** Releases transient cache snapshot banks while preserving paths, identity, and flags. */
void release_scratch_locked(Context& state) noexcept;

/**
 * Gives mutable views over every generated domain.
 * @param state Its lock must already be held exclusively.
 * @return Mutable spans covering each full-size domain buffer.
 */
[[nodiscard]] cache::records::MutableDomains scratch_domains(Context& state) noexcept;

/**
 * Gives read-only views over the used rows of one canonical snapshot.
 * @param state Its lock must already be held exclusively.
 * @param counts Used row count for each fixed domain buffer.
 * @return Read-only spans limited to the used rows.
 */
[[nodiscard]] cache::records::Domains
occupied_domains(Context& state, const cache::records::DomainCounts& counts) noexcept;

/**
 * Saves one canonical snapshot when all domains required by its build are ready.
 * @param state Its lock must already be held exclusively.
 * @param catalystRequired True when the snapshot must contain the catalyst catalog.
 * @return True when no write is due, or the required State is on disk.
 */
[[nodiscard]] bool persist_if_ready_locked(Context& state, bool catalystRequired) noexcept;

} // namespace sunrise::state::build_data::runtime::persistence
