#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "../../abilities/ability_bucket_catalog.h"
#include "../../hash_names/hash_name_catalog.h"
#include "../../inventory/buckets/inventory_bucket_catalog.h"
#include "../../items/details/item_detail_catalog.h"
#include "../../progressions/progression_catalog.h"
#include "../../scenarios/scenario_catalog.h"
#include "../../socket_entry_lists/socket_entry_list_catalog.h"
#include "../../spawn_sets/spawn_set_catalog.h"
#include "validation.h"

namespace sunrise::state::build_data::cache::records {
namespace {

/** @return The row's name, limited to its stored length. */
[[nodiscard]] std::string_view name_of(const content::Definition& value) noexcept {
    return {value.name.data(), value.nameLength};
}

/** @return True when the row's name is safe to encode. */
[[nodiscard]] bool valid_name(const content::Definition& value) noexcept {
    if (value.nameLength == 0 || value.nameLength >= value.name.size()) {
        return false;
    }
    const auto logicalName = std::span(value.name).first(value.nameLength);
    return std::find(logicalName.begin(), logicalName.end(), '\0') == logicalName.end();
}

/** @return Name order, so the sweep cannot change the file. */
[[nodiscard]] bool scenario_less(const scenarios::Definition& left,
                                 const scenarios::Definition& right) noexcept {
    return std::string_view(left.name.data(), left.nameLength)
           < std::string_view(right.name.data(), right.nameLength);
}

/** @return Standard field order for named rows. */
[[nodiscard]] bool named_less(const content::Definition& left,
                              const content::Definition& right) noexcept {
    const std::string_view leftName = name_of(left);
    const std::string_view rightName = name_of(right);
    if (leftName != rightName) {
        return leftName < rightName;
    }
    if (left.tag != right.tag) {
        return left.tag < right.tag;
    }
    return left.classId < right.classId;
}

/** @return Native-index order for detail rows. */
[[nodiscard]] bool detail_less(const items::details::Definition& left,
                               const items::details::Definition& right) noexcept {
    return left.definitionIndex < right.definitionIndex;
}

/** @return Native bucket-id order. */
[[nodiscard]] bool bucket_less(const inventory::buckets::Descriptor& left,
                               const inventory::buckets::Descriptor& right) noexcept {
    return left.bucketId < right.bucketId;
}

/** @return Native definition-index order for item rows. */
[[nodiscard]] bool item_less(const items::Definition& left,
                             const items::Definition& right) noexcept {
    return left.definitionIndex < right.definitionIndex;
}

/** @return Native-index order for socket-list rows. */
[[nodiscard]] bool socket_less(const socket_entry_lists::Definition& left,
                               const socket_entry_lists::Definition& right) noexcept {
    return left.definitionIndex < right.definitionIndex;
}

/** @return Native-index order for entry-table rows. */
[[nodiscard]] bool socket_table_less(const socket_entry_lists::EntryTable& left,
                                     const socket_entry_lists::EntryTable& right) noexcept {
    return left.definitionIndex < right.definitionIndex;
}

/** @return Subclass order, then selected-entry order, movement first. */
[[nodiscard]] bool ability_less(const abilities::Definition& left,
                                const abilities::Definition& right) noexcept {
    if (left.socketEntryListIndex != right.socketEntryListIndex) {
        return left.socketEntryListIndex < right.socketEntryListIndex;
    }
    const auto key = [](const abilities::Selection& selection) noexcept {
        return std::array<std::uint8_t, 5>{selection.movementEntry,
                                           selection.grenadeEntry,
                                           selection.superEntry,
                                           selection.meleeEntry,
                                           selection.classEntry};
    };
    return key(left.selection) < key(right.selection);
}

/** @return True when every row sorts strictly before the next, so no two are equal. */
template <typename Value, typename Less>
[[nodiscard]] bool strictly_ordered(std::span<const Value> values, Less less) noexcept {
    return std::adjacent_find(
               values.begin(),
               values.end(),
               [&less](const Value& left, const Value& right) { return !less(left, right); })
           == values.end();
}

/** @return True when every count fits the fixed storage. */
[[nodiscard]] bool counts_fit(MutableDomains domains, const DomainCounts& counts) noexcept {
    return counts.named <= domains.named.size() && counts.items <= domains.items.size()
           && counts.itemDetails <= domains.itemDetails.size()
           && counts.inventoryBuckets <= domains.inventoryBuckets.size()
           && counts.socketEntryLists <= domains.socketEntryLists.size()
           && counts.socketEntryTables <= domains.socketEntryTables.size()
           && counts.abilityBuckets <= domains.abilityBuckets.size()
           && counts.progressions <= domains.progressions.size()
           && counts.scenarios <= domains.scenarios.size()
           && counts.rosterGroups <= domains.rosterGroups.size()
           && counts.spawnStems <= domains.spawnStems.size()
           && counts.spawnNameHashes <= domains.spawnNameHashes.size()
           && counts.hashNames <= domains.hashNames.size();
}

} // namespace

/** Sorts every filled domain before the checks, so the written file is always the same. */
bool canonicalize(MutableDomains domains, const DomainCounts& counts) noexcept {
    if (!counts_fit(domains, counts)) {
        return false;
    }
    const auto named = domains.named.first(counts.named);
    const auto items = domains.items.first(counts.items);
    const auto itemDetails = domains.itemDetails.first(counts.itemDetails);
    const auto inventoryBuckets = domains.inventoryBuckets.first(counts.inventoryBuckets);
    const auto socketEntryLists = domains.socketEntryLists.first(counts.socketEntryLists);
    std::sort(named.begin(), named.end(), named_less);
    std::sort(items.begin(), items.end(), item_less);
    std::sort(itemDetails.begin(), itemDetails.end(), detail_less);
    std::sort(inventoryBuckets.begin(), inventoryBuckets.end(), bucket_less);
    const auto abilityBuckets = domains.abilityBuckets.first(counts.abilityBuckets);
    const auto socketEntryTables = domains.socketEntryTables.first(counts.socketEntryTables);
    std::sort(socketEntryLists.begin(), socketEntryLists.end(), socket_less);
    std::sort(socketEntryTables.begin(), socketEntryTables.end(), socket_table_less);
    std::sort(abilityBuckets.begin(), abilityBuckets.end(), ability_less);
    const auto scenarioRows = domains.scenarios.first(counts.scenarios);
    std::sort(scenarioRows.begin(), scenarioRows.end(), scenario_less);
    return true;
}

/** Checks the structure rules, the sort order, and every cross-domain item reference. */
bool valid_domains(Domains domains) noexcept {
    if (domains.constants.extracted != 1U || domains.named.empty() || domains.items.empty()
        || domains.inventoryBuckets.empty() || domains.socketEntryLists.empty()
        || !std::all_of(domains.named.begin(), domains.named.end(), valid_name)
        || !strictly_ordered(domains.named, named_less) || !items::valid(domains.items)
        || !inventory::buckets::valid(domains.inventoryBuckets)
        || !strictly_ordered(domains.inventoryBuckets, bucket_less)
        || !socket_entry_lists::valid(domains.socketEntryLists)
        || !socket_entry_lists::valid_entry_tables(domains.socketEntryTables)
        || !strictly_ordered(domains.itemDetails, detail_less)
        || !abilities::valid(domains.abilityBuckets)
        || !strictly_ordered(domains.abilityBuckets, ability_less)
        || !progressions::valid(domains.progressions)
        || !scenarios::valid(domains.scenarios, domains.rosterGroups)
        // An empty catalog is complete. It is what a build with no installed spawn set means.
        // Both arrays must be empty together, because a stem names its hashes by range.
        || (domains.spawnStems.empty()
                ? !domains.spawnNameHashes.empty()
                : !spawn_sets::valid(domains.spawnStems, domains.spawnNameHashes))
        || !hash_names::valid(domains.hashNames)) {
        return false;
    }
    for (std::size_t index = 0; index < domains.items.size(); ++index) {
        if (domains.items[index].definitionIndex != index) {
            return false;
        }
    }
    for (std::size_t index = 0; index < domains.socketEntryLists.size(); ++index) {
        if (domains.socketEntryLists[index].definitionIndex != index) {
            return false;
        }
    }
    return valid_item_detail_links(
        domains.itemDetails, domains.items, domains.inventoryBuckets, domains.socketEntryLists);
}

} // namespace sunrise::state::build_data::cache::records
