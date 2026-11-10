#include <algorithm>
#include <array>

#include "../runtime.h"
#include "../runtime/persistence/publication_transaction.h"
#include "../vendors/repeatable_triggers.h"
#include "bounty_catalog.h"

namespace sunrise::state::build_data {

/** @return True when the repeatable bounty table is published. */
bool repeatable_bounties_ready() noexcept {
    return bounties::count() != 0;
}

/** Publishes every repeatable bounty and the item-type its pool is keyed by. */
bool publish_repeatable_bounties(std::span<const bounties::Definition> definitions) noexcept {
    runtime::persistence::Transaction transaction;
    return transaction.active()
           && transaction.finish(bounties::replace(definitions), bounties::clear);
}

/** Lists the item indices one repeatable vendor category rolls from. */
bool repeatable_bounty_pool(const bounties::ItemType& itemType,
                            std::span<std::uint16_t> output,
                            std::size_t& count) noexcept {
    count = 0;
    return repeatable_bounties_ready() && bounties::pool(itemType, output, count);
}

/** Walks the authored trigger rows, whose item-types key the extracted pools. */
bool in_repeatable_bounty_pool(std::uint16_t itemIndex) noexcept {
    std::array<std::uint16_t, bounties::kPoolCapacity> pool{};
    for (const vendors::RepeatableTrigger& trigger : vendors::kRepeatableTriggers) {
        std::size_t count = 0;
        if (!repeatable_bounty_pool(trigger.itemType, pool, count)) {
            continue;
        }
        const auto members = std::span(pool).first(count);
        if (std::find(members.begin(), members.end(), itemIndex) != members.end()) {
            return true;
        }
    }
    return false;
}

} // namespace sunrise::state::build_data
