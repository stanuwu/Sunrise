#pragma once

#include <Windows.h>

#include <array>

#include "../../../../core/filesystem/path.h"
#include "../../../content/content_catalog.h"
#include "../../abilities/definition.h"
#include "../../cache/records/domains.h"
#include "../../constants/definition.h"
#include "../../definition.h"
#include "../../hash_names/definition.h"
#include "../../inventory/buckets/definition.h"
#include "../../items/details/definition.h"
#include "../../items/item_catalog.h"
#include "../../progressions/definition.h"
#include "../../scenarios/definition.h"
#include "../../socket_entry_lists/definition.h"
#include "../../spawn_sets/definition.h"

namespace sunrise::state::build_data::runtime::persistence {

/** Fixed cache paths, identity, and canonical snapshot storage guarded by one State lock. */
struct Context {
    SRWLOCK lock{SRWLOCK_INIT};
    std::array<content::Definition, content::kDefinitionCatalogCapacity> namedScratch{};
    std::array<items::Definition, items::kDefinitionCapacity> itemScratch{};
    std::array<items::details::Definition, items::details::kDefinitionCapacity> itemDetailScratch{};
    std::array<inventory::buckets::Descriptor, inventory::buckets::kDescriptorCapacity>
        inventoryBucketScratch{};
    std::array<socket_entry_lists::Definition, socket_entry_lists::kDefinitionCapacity>
        socketEntryListScratch{};
    std::array<socket_entry_lists::EntryTable, socket_entry_lists::kEntryTableCapacity>
        socketEntryTableScratch{};
    std::array<abilities::Definition, abilities::kDefinitionCapacity> abilityBucketScratch{};
    std::array<progressions::Definition, progressions::kDefinitionCapacity> progressionScratch{};
    std::array<scenarios::Definition, scenarios::kDefinitionCapacity> scenarioScratch{};
    std::array<scenarios::RosterGroup, scenarios::kRosterGroupCapacity> rosterGroupScratch{};
    std::array<spawn_sets::Stem, spawn_sets::kStemCapacity> spawnStemScratch{};
    std::array<spawn_sets::NameHash, spawn_sets::kNameHashCapacity> spawnNameHashScratch{};
    std::array<hash_names::Name, hash_names::kNameCapacity> hashNameScratch{};
    cache::records::InvestmentConstants constantsScratch{};
    core::path::Buffer cacheDirectory;
    core::path::Buffer cachePath;
    BuildIdentity buildIdentity{};
    bool enabled{};
    bool persisted{};
    bool replaceStaleCache{};
};

/** @return The process-wide persistence context, shared by lifecycle and writer code. */
[[nodiscard]] Context& context() noexcept;

/** @return True when every generated domain is complete in State. */
[[nodiscard]] bool all_domains_ready() noexcept;

/**
 * Clears fixed cache paths, identity, flags, and snapshot storage.
 * @param state Its lock must already be held exclusively.
 */
void clear_locked(Context& state) noexcept;

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
 * Saves one complete canonical snapshot when every domain is ready.
 * @param state Its lock must already be held exclusively.
 * @return True when no write is due, or the complete State is on disk.
 */
[[nodiscard]] bool persist_if_complete_locked(Context& state) noexcept;

} // namespace sunrise::state::build_data::runtime::persistence
