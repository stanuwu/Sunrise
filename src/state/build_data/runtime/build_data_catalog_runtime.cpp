#include "build_data_catalog_runtime.h"

#include "../../content/content_catalog.h"
#include "../abilities/ability_bucket_catalog.h"
#include "../constants/investment_constant_catalog.h"
#include "../hash_names/hash_name_catalog.h"
#include "../inventory/buckets/inventory_bucket_catalog.h"
#include "../items/details/item_detail_catalog.h"
#include "../progressions/progression_catalog.h"
#include "../runtime.h"
#include "../scenarios/scenario_catalog.h"
#include "../socket_entry_lists/socket_entry_list_catalog.h"
#include "../spawn_sets/spawn_set_catalog.h"
#include "domain_markers.h"
#include "persistence/publication_transaction.h"

namespace sunrise::state::build_data {
namespace {

/**
 * Checks configured detail references against the published numeric catalogs.
 * @param definitions Candidate configured item details.
 * @return True when every item, plug, bucket, and socket-list reference is found.
 */
[[nodiscard]] bool
valid_detail_publication(std::span<const items::details::Definition> definitions) noexcept {
    const std::size_t itemCount = items::count();
    const std::size_t socketCount = socket_entry_lists::count();
    if (itemCount == 0 || socketCount == 0 || inventory::buckets::count() == 0
        || (!definitions.empty() && !items::details::valid(definitions))) {
        return false;
    }
    // Plug rows share the domain with equipped rows and cannot be equipped, so the equipment slot
    // and the bucket routing are checked where a loadout uses them, not here.
    for (const items::details::Definition& definition : definitions) {
        items::Definition item{};
        if (definition.definitionIndex >= itemCount
            || definition.socketEntryListIndex >= socketCount
            || !items::find_index(definition.definitionIndex, item)
            || item.bucketId != definition.bucketId) {
            return false;
        }
        for (const std::uint16_t plugIndex : definition.initialPlugIndices) {
            if (plugIndex != items::details::kUnavailableItemIndex && plugIndex >= itemCount) {
                return false;
            }
        }
    }
    return true;
}

/** Drops the ability ready flag, then removes the failed ability bucket candidate. */
void rollback_ability_publication() noexcept {
    runtime::ability_buckets::clear();
    abilities::clear();
}

/** Drops the bubble-name ready flag, then removes the failed bubble-name candidate. */
void rollback_name_catalog_publication() noexcept {
    runtime::name_catalog::clear();
    hash_names::clear();
}

/** Drops the spawn-set ready flag, then removes the failed spawn-set candidate. */
void rollback_spawn_catalog_publication() noexcept {
    runtime::spawn_catalog::clear();
    spawn_sets::clear();
}

/** Drops the detail ready flag, then removes the failed configured-detail candidate. */
void rollback_detail_publication() noexcept {
    runtime::details::clear();
    items::details::clear();
}

} // namespace

/** @return True when a complete configured-detail domain, empty or not, is published. */
bool configured_item_details_ready() noexcept {
    return runtime::details::ready();
}

/** Publishes configured details, empty or not, once the numeric domains are ready. */
bool publish_configured_item_details(
    std::span<const items::details::Definition> definitions) noexcept {
    runtime::persistence::Transaction transaction;
    if (!transaction.active() || !valid_detail_publication(definitions)) {
        return false;
    }
    if (definitions.empty()) {
        items::details::clear();
    } else if (!items::details::replace(definitions)) {
        return false;
    }
    // Row count does not matter here, because an empty loadout is a complete one.
    runtime::details::publish();
    return transaction.finish(true, rollback_detail_publication);
}

/** Finds one configured item detail, once the whole table is in State. */
bool find_configured_item_detail(std::uint16_t definitionIndex,
                                 items::details::Definition& definition) noexcept {
    definition = {};
    return configured_item_details_ready() && items::details::find(definitionIndex, definition);
}

/** @return True when the whole progression definition table is in State. */
bool progression_definitions_ready() noexcept {
    return progressions::count() != 0;
}

/** Publishes the whole progression definition table in one step. */
bool publish_progression_definitions(
    std::span<const progressions::Definition> definitions) noexcept {
    runtime::persistence::Transaction transaction;
    return transaction.active()
           && transaction.finish(progressions::replace(definitions), progressions::clear);
}

/** @return True when a complete destination-layout domain, empty or not, is published. */
bool scenario_layouts_ready() noexcept {
    return scenarios::count() != 0;
}

/** Publishes the extracted destination layouts and their roster groups in one step. */
bool publish_scenario_layouts(std::span<const scenarios::Definition> definitions,
                              std::span<const scenarios::RosterGroup> groups) noexcept {
    runtime::persistence::Transaction transaction;
    return transaction.active()
           && transaction.finish(scenarios::replace(definitions, groups), scenarios::clear);
}

/** @return Number of published destination layouts, read under the lock. */
std::size_t scenario_layout_count() noexcept {
    return scenarios::count();
}

/** Copies every published destination layout. */
bool snapshot_scenario_layouts(std::span<scenarios::Definition> output,
                               std::size_t& count) noexcept {
    count = 0;
    return scenario_layouts_ready() && scenarios::snapshot(output, count);
}

/** @return True when a complete bubble-name table, empty or not, is published. */
bool hash_names_ready() noexcept {
    return runtime::name_catalog::ready();
}

/** Publishes the resolved bubble names in one step. */
bool publish_hash_names(std::span<const hash_names::Name> names) noexcept {
    runtime::persistence::Transaction transaction;
    if (!transaction.active()) {
        return false;
    }
    if (!hash_names::replace(names)) {
        return transaction.finish(false, rollback_name_catalog_publication);
    }
    runtime::name_catalog::publish();
    return transaction.finish(true, rollback_name_catalog_publication);
}

/** Finds one bubble's internal name by its hash. */
bool find_hash_name(std::uint32_t hash, hash_names::Name& name) noexcept {
    name = {};
    return hash_names_ready() && hash_names::find(hash, name);
}

/** @return True when a complete spawn-set catalog, empty or not, is published. */
bool spawn_sets_ready() noexcept {
    return runtime::spawn_catalog::ready();
}

/** Publishes the spawn-set catalog extracted from the installed packages, in one step. */
bool publish_spawn_sets(std::span<const spawn_sets::Stem> stems,
                        std::span<const spawn_sets::NameHash> nameHashes) noexcept {
    runtime::persistence::Transaction transaction;
    if (!transaction.active()) {
        return false;
    }
    // An empty catalog is complete. It is what a build with no installed spawn set means.
    const bool replaced =
        stems.empty() ? nameHashes.empty() : spawn_sets::replace(stems, nameHashes);
    if (!replaced) {
        return transaction.finish(false, rollback_spawn_catalog_publication);
    }
    runtime::spawn_catalog::publish();
    return transaction.finish(true, rollback_spawn_catalog_publication);
}

/** Copies the whole flat spawn-name hash bank. */
bool snapshot_spawn_name_hashes(std::span<spawn_sets::NameHash> output,
                                std::size_t& count) noexcept {
    count = 0;
    return spawn_sets_ready() && spawn_sets::snapshot_hashes(output, count);
}

/** Finds the spawn sets one map-package stem declares. */
bool find_spawn_sets(std::string_view stem,
                     std::span<spawn_sets::NameHash> output,
                     std::size_t& count) noexcept {
    count = 0;
    spawn_sets::Stem row{};
    return spawn_sets_ready() && spawn_sets::find(stem, row)
           && spawn_sets::stem_hashes(row, output, count);
}

/** Finds one destination's bubble layout by package name. */
bool find_scenario_layout(std::string_view name, scenarios::Definition& definition) noexcept {
    definition = {};
    return scenario_layouts_ready() && scenarios::find(name, definition);
}

/** Lists the definition index each slot of one scope's progression array carries. */
bool find_progression_slots(progressions::Scope scope,
                            std::span<std::uint16_t> output,
                            std::size_t& count) noexcept {
    count = 0;
    return progression_definitions_ready() && progressions::slots(scope, output, count);
}

/** @return True when a complete ability bucket domain, empty or not, is published. */
bool ability_buckets_ready() noexcept {
    return runtime::ability_buckets::ready();
}

/** Publishes the ability buckets every configured subclass and ability selection publishes. */
bool publish_ability_buckets(std::span<const abilities::Definition> definitions) noexcept {
    runtime::persistence::Transaction transaction;
    if (!transaction.active() || !abilities::replace(definitions)) {
        return false;
    }
    // Row count does not matter here, because a loadout with no subclass is a complete one.
    runtime::ability_buckets::publish();
    return transaction.finish(true, rollback_ability_publication);
}

/** Finds the buckets one subclass publishes under one ability selection. */
bool find_ability_buckets(std::uint16_t socketEntryListIndex,
                          const abilities::Selection& selection,
                          abilities::Definition& definition) noexcept {
    definition = {};
    return ability_buckets_ready() && abilities::find(socketEntryListIndex, selection, definition);
}

/** @return True when the installed investment constants are in State. */
bool investment_constants_ready() noexcept {
    constants::InvestmentConstants published{};
    return constants::find(published);
}

/** Publishes the stat rows read from the installed investment constants blob. */
bool publish_investment_constants(const constants::InvestmentConstants& value) noexcept {
    runtime::persistence::Transaction transaction;
    return transaction.active() && transaction.finish(constants::replace(value), constants::clear);
}

/** Reads the published investment constants. */
bool find_investment_constants(constants::InvestmentConstants& value) noexcept {
    return constants::find(value);
}

namespace runtime {

/** Clears every generated catalog and the configured-domain publication state. */
void clear_catalogs() noexcept {
    content::clear();
    named::clear();
    items::clear();
    items::details::clear();
    details::clear();
    inventory::buckets::clear();
    socket_entry_lists::clear();
    rollback_ability_publication();
    progressions::clear();
    scenarios::clear();
    rollback_spawn_catalog_publication();
    rollback_name_catalog_publication();
    constants::clear();
}

} // namespace runtime

} // namespace sunrise::state::build_data
