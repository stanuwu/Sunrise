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
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (!names_installed_item(rows[index])) {
            core::log::writef(core::log::Channel::state,
                              core::log::Level::warn,
                              "ev=pursuit_definitions result=rejected item=%u row=%zu total=%zu",
                              static_cast<unsigned>(rows[index].definitionIndex),
                              index,
                              rows.size());
            return false;
        }
    }
    return pursuits::valid(rows, objectives::count());
}

/** Publishes the contracts once every edge is checked. */
bool publish_pursuit_definitions(std::span<const pursuits::Definition> rows) noexcept {
    if (!valid_pursuit_definitions(rows)) {
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
