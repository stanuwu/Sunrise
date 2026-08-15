#pragma once

#include <cstddef>
#include <span>

#include "../../../content/content_catalog.h"
#include "../../abilities/definition.h"
#include "../../hash_names/definition.h"
#include "../../inventory/buckets/definition.h"
#include "../../items/details/definition.h"
#include "../../items/item_catalog.h"
#include "../../progressions/definition.h"
#include "../../scenarios/definition.h"
#include "../../socket_entry_lists/definition.h"
#include "../../spawn_sets/definition.h"
#include "format.h"

namespace sunrise::state::build_data::cache::records {

/** Checked row counts for every generated cache domain. */
struct DomainCounts {
    std::size_t named{};
    std::size_t items{};
    std::size_t itemDetails{};
    std::size_t inventoryBuckets{};
    std::size_t socketEntryLists{};
    std::size_t socketEntryTables{};
    std::size_t abilityBuckets{};
    std::size_t progressions{};
    std::size_t scenarios{};
    std::size_t rosterGroups{};
    std::size_t spawnStems{};
    std::size_t spawnNameHashes{};
    std::size_t hashNames{};
};

/** Fixed caller storage used while decoding the cache domains. */
struct MutableDomains {
    /** Header scalars. The reader writes these straight through; no record array holds them. */
    InvestmentConstants* constants{};
    std::span<content::Definition> named;
    std::span<items::Definition> items;
    std::span<items::details::Definition> itemDetails;
    std::span<inventory::buckets::Descriptor> inventoryBuckets;
    std::span<socket_entry_lists::Definition> socketEntryLists;
    std::span<socket_entry_lists::EntryTable> socketEntryTables;
    std::span<abilities::Definition> abilityBuckets;
    std::span<progressions::Definition> progressions;
    std::span<scenarios::Definition> scenarios;
    std::span<scenarios::RosterGroup> rosterGroups;
    std::span<spawn_sets::Stem> spawnStems;
    std::span<spawn_sets::NameHash> spawnNameHashes;
    std::span<hash_names::Name> hashNames;
};

/** Read-only complete views used for the checks and for cache encoding. */
struct Domains {
    InvestmentConstants constants{};
    std::span<const content::Definition> named;
    std::span<const items::Definition> items;
    std::span<const items::details::Definition> itemDetails;
    std::span<const inventory::buckets::Descriptor> inventoryBuckets;
    std::span<const socket_entry_lists::Definition> socketEntryLists;
    std::span<const socket_entry_lists::EntryTable> socketEntryTables;
    std::span<const abilities::Definition> abilityBuckets;
    std::span<const progressions::Definition> progressions;
    std::span<const scenarios::Definition> scenarios;
    std::span<const scenarios::RosterGroup> rosterGroups;
    std::span<const spawn_sets::Stem> spawnStems;
    std::span<const spawn_sets::NameHash> spawnNameHashes;
    std::span<const hash_names::Name> hashNames;
};

} // namespace sunrise::state::build_data::cache::records
