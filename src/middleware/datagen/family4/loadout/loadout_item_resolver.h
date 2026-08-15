#pragma once

#include <cstddef>
#include <cstdint>

#include "../../../../state/account/account_state.h"
#include "../../../../state/account/inventory/inventory_state.h"
#include "../../../../state/build_data/inventory/buckets/definition.h"
#include "definition.h"

namespace sunrise::middleware::datagen::family4::loadout {

/** Resolved item plus its runtime bucket range, retained only until row placement. */
struct Candidate {
    ResolvedItem item{};
    state::build_data::inventory::buckets::Descriptor bucket{};
};

/**
 * Resolves one authored item into native mappings without choosing an inventory row.
 * @param authored Semantic equipment item from State.
 * @param character Authored character that owns the item.
 * @param itemDefinitionCount Stable dense item-table row count.
 * @param socketEntryListCount Stable dense socket-list row count.
 * @param output Receives a complete candidate only on success.
 * @return True when every base, plug, bucket, and initial socket mapping resolves.
 */
[[nodiscard]] bool resolve_item(const state::account::inventory::Item& authored,
                                const state::CharacterState& character,
                                std::size_t itemDefinitionCount,
                                std::size_t socketEntryListCount,
                                Candidate& output) noexcept;

} // namespace sunrise::middleware::datagen::family4::loadout
