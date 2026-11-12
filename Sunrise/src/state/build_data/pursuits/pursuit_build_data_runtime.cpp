#include <algorithm>
#include <span>

#include "../../../core/logging/log.h"
#include "../objectives/objective_catalog.h"
#include "../runtime.h"
#include "../runtime/persistence/publication_transaction.h"
#include "pursuit_catalog.h"

namespace sunrise::state::build_data {
namespace {

/** Each contract must name the installed item it was read from. */
[[nodiscard]] bool names_installed_item(const pursuits::Definition& row) noexcept {
    items::Definition item{};
    return find_item_definition_index(row.definitionIndex, item)
           && item.definitionHash == row.definitionHash;
}

/** Its reward rows and the quest step after it must name installed items. */
[[nodiscard]] bool references_installed_items(const pursuits::Definition& row) noexcept {
    items::Definition item{};
    if (row.rewardCount > row.rewards.size()) {
        return false;
    }
    const auto rewards = std::span(row.rewards).first(row.rewardCount);
    return std::all_of(rewards.begin(),
                       rewards.end(),
                       [&item](const pursuits::RewardPreview& reward) {
                           return find_item_definition_index(reward.itemIndex, item);
                       })
           && (row.questStep.successorIndex == pursuits::kAbsent
               || find_item_definition_index(row.questStep.successorIndex, item));
}

/** @return The first contract naming an item the catalog lacks, or rows.size() when none does. */
[[nodiscard]] std::size_t first_uninstalled(std::span<const pursuits::Definition> rows) noexcept {
    const auto found = std::find_if(rows.begin(), rows.end(), [](const pursuits::Definition& row) {
        return !names_installed_item(row) || !references_installed_items(row);
    });
    return static_cast<std::size_t>(found - rows.begin());
}

/** Drops both tables when the first cache write after settling fails. */
void clear_pursuit_catalog() noexcept {
    objectives::clear();
    pursuits::clear();
}

} // namespace

/** @return True when the pursuit contracts are published. */
bool pursuit_definitions_ready() noexcept {
    return pursuits::ready();
}

/** @return True once the installed data has settled that no contract can be read. */
bool pursuit_catalog_unsupported() noexcept {
    return pursuits::unsupported();
}

/** @return A generation that changes after every publication, settlement or clear. */
std::uint64_t pursuit_definitions_revision() noexcept {
    return pursuits::revision();
}

/** Requires both tables and logs the first contract whose item differs from the installed one. */
bool valid_pursuit_definitions(std::span<const pursuits::Definition> rows) noexcept {
    if (!item_definitions_ready() || !objective_definitions_ready()) {
        return false;
    }
    const std::size_t index = first_uninstalled(rows);
    if (index != rows.size()) {
        core::log::writef(core::log::Channel::state,
                          core::log::Level::warn,
                          "ev=build_data stage=pursuits result=rejected item=%u row=%zu total=%zu",
                          static_cast<unsigned>(rows[index].definitionIndex),
                          index,
                          rows.size());
        return false;
    }
    return pursuits::valid(rows, objectives::count());
}

/** Publishes the contracts once every edge is checked; the catalog checks their own fields. */
bool publish_pursuit_definitions(std::span<const pursuits::Definition> rows) noexcept {
    if (!item_definitions_ready() || !objective_definitions_ready()
        || first_uninstalled(rows) != rows.size()) {
        return false;
    }
    runtime::persistence::Transaction transaction;
    return transaction.active()
           && transaction.finish(pursuits::replace(rows, objectives::count()), pursuits::clear);
}

/** Withdraws the objective table too, since only contracts read it. */
void settle_pursuit_catalog_unsupported() noexcept {
    runtime::persistence::Transaction transaction;
    if (!transaction.active()) {
        return;
    }
    objectives::clear();
    pursuits::settle_unsupported();
    static_cast<void>(transaction.finish(true, clear_pursuit_catalog));
}

/** Copies one item's completion contract. */
bool find_pursuit_definition(std::uint16_t index, pursuits::Definition& output) noexcept {
    return pursuits::find(index, output);
}

} // namespace sunrise::state::build_data
