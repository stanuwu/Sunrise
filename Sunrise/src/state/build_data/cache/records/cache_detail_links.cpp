#include <algorithm>

#include "../../items/catalysts/exotic_catalyst_builder.h"
#include "../../items/details/item_detail_catalog.h"
#include "../../items/socket_plugs/socket_plug_catalog.h"
#include "validation.h"

namespace sunrise::state::build_data::cache::records {

/** Checks collectible ordinals and their optional links into the installed item table. */
bool valid_collectible_links(std::span<const collectibles::Definition> collectibleDefinitions,
                             std::span<const items::Definition> itemDefinitions) noexcept {
    if (collectibleDefinitions.empty() || itemDefinitions.empty()
        || !collectibles::valid(collectibleDefinitions)) {
        return false;
    }
    for (std::size_t index = 0; index < collectibleDefinitions.size(); ++index) {
        const collectibles::Definition& definition = collectibleDefinitions[index];
        if (definition.collectibleIndex != index
            || (definition.itemDefinitionIndex != collectibles::kUnavailableItemDefinitionIndex
                && (definition.itemDefinitionIndex >= itemDefinitions.size()
                    || itemDefinitions[definition.itemDefinitionIndex].definitionIndex
                           != definition.itemDefinitionIndex))) {
            return false;
        }
        for (std::size_t requirementIndex = 0;
             requirementIndex < definition.materialRequirementCount;
             ++requirementIndex) {
            const collectibles::MaterialRequirement& requirement =
                definition.materialRequirements[requirementIndex];
            if (requirement.itemDefinitionIndex >= itemDefinitions.size()
                || itemDefinitions[requirement.itemDefinitionIndex].definitionIndex
                       != requirement.itemDefinitionIndex) {
                return false;
            }
        }
    }
    return true;
}

/**
 * Checks every supported detail reference against the other numeric domains.
 * @param details Item
 * details that already passed their own checks.
 * @param itemDefinitions Complete dense item rows.
 * @param inventoryBuckets Complete bucket-routing rows in bucket order.
 * @param socketEntryLists Complete dense socket-list rows.
 * @return True when every detail reference points inside the other domains.
 */
bool valid_item_detail_links(
    std::span<const items::details::Definition> details,
    std::span<const items::Definition> itemDefinitions,
    std::span<const inventory::buckets::Descriptor> inventoryBuckets,
    std::span<const socket_entry_lists::Definition> socketEntryLists) noexcept {
    if (itemDefinitions.empty() || inventoryBuckets.empty() || socketEntryLists.empty()) {
        return false;
    }
    if (details.empty()) {
        return true;
    }
    if (!items::details::valid(details)) {
        return false;
    }
    // The domain holds equippable items and the plugs they socket. Only the base rows can be
    // equipped, and the loadout resolver already requires that there, so it is not a domain rule.
    for (const items::details::Definition& detail : details) {
        if (detail.definitionIndex >= itemDefinitions.size()
            || detail.socketEntryListIndex >= socketEntryLists.size()
            || itemDefinitions[detail.definitionIndex].bucketId != detail.bucketId) {
            return false;
        }
        for (const std::uint16_t plugIndex : detail.initialPlugIndices) {
            if (plugIndex != items::details::kUnavailableItemIndex
                && plugIndex >= itemDefinitions.size()) {
                return false;
            }
        }
    }
    return true;
}

/** Checks every socket target lane and allowed plug against the numeric item domains. */
bool valid_socket_plug_links(std::span<const items::socket_plugs::Rule> rules,
                             std::span<const items::socket_plugs::Pool> pools,
                             std::span<const items::socket_plugs::Member> members,
                             std::span<const items::Definition> itemDefinitions,
                             std::span<const items::details::Definition> details) noexcept {
    if (itemDefinitions.empty() || details.empty()
        || !items::socket_plugs::valid(rules, pools, members)) {
        return false;
    }
    for (const items::socket_plugs::Rule& rule : rules) {
        if (rule.itemDefinitionIndex >= itemDefinitions.size()) {
            return false;
        }
        const auto found = std::lower_bound(
            details.begin(),
            details.end(),
            rule.itemDefinitionIndex,
            [](const items::details::Definition& detail, std::uint16_t definitionIndex) {
                return detail.definitionIndex < definitionIndex;
            });
        if (found == details.end() || found->definitionIndex != rule.itemDefinitionIndex
            || rule.lane >= found->ordinarySocketCount) {
            return false;
        }
    }
    return std::all_of(members.begin(), members.end(), [&itemDefinitions](const auto member) {
        return member < itemDefinitions.size();
    });
}

/** Checks each catalyst against its item, detail, pool, plug, and pinned release state. */
bool valid_exotic_catalyst_links(const BuildIdentity& build, Domains domains) noexcept {
    const items::catalysts::Facts facts = items::catalysts::generated_facts();
    if (!items::catalysts::supports_build(build, facts)) {
        return domains.exoticCatalysts.empty();
    }
    if (domains.items.empty() || domains.itemDetails.empty() || domains.exoticCatalysts.empty()
        || !items::socket_plugs::valid(
            domains.socketPlugRules, domains.socketPlugPools, domains.socketPlugMembers)) {
        return false;
    }
    const items::catalysts::Source source{
        build,
        domains.items,
        domains.itemDetails,
        domains.socketPlugRules,
        domains.socketPlugPools,
        domains.socketPlugMembers,
        {},
        {},
        {},
    };
    return items::catalysts::matches_cached(source, facts, domains.exoticCatalysts);
}

} // namespace sunrise::state::build_data::cache::records
