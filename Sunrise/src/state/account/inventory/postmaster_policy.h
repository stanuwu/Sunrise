#pragma once

#include "../../build_data/inventory/buckets/definition.h"
#include "../../build_data/items/details/definition.h"
#include "inventory_state.h"

namespace sunrise::state::account::inventory {

/** Lost Items retains whole equipment and engram instances without changing their contents. */
[[nodiscard]] inline bool
lost_items_supported(const Item& item,
                     const build_data::items::details::Definition& detail) noexcept {
    const bool equipment = detail.equipmentSlot.has_value() && *detail.equipmentSlot > 0;
    const bool engram = detail.bucketId == build_data::inventory::buckets::kEngramBucketId
                        && !detail.equipmentSlot.has_value();
    return valid(item) && item.quantity == 1 && detail.maxStackSize == 1
           && detail.instancedDefinitionState
                  == build_data::items::details::InstancedDefinitionState::instanced
           && (equipment || engram);
}

} // namespace sunrise::state::account::inventory
