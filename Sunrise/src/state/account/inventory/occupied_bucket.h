#pragma once

#include <cstdint>

#include "../../build_data/runtime.h"
#include "inventory_state.h"

namespace sunrise::state::account::inventory {

/** @return The id of the bucket a saved row of this item occupies under its placement. */
[[nodiscard]] inline std::uint8_t occupied_bucket_id(const build_data::items::Definition& item,
                                                     ItemPlacement placement) noexcept {
    return placement == ItemPlacement::recovery ? item.recoveryBucketId : item.bucketId;
}

/** Finds the installed bucket a saved row of this item occupies under its placement. */
[[nodiscard]] inline bool
find_occupied_bucket(const build_data::items::Definition& item,
                     ItemPlacement placement,
                     build_data::inventory::buckets::Descriptor& bucket) noexcept {
    return build_data::find_inventory_bucket_descriptor(occupied_bucket_id(item, placement),
                                                        bucket);
}

/**
 * Names the placement a row of this item takes in a bucket: its authored bucket, else its recovery
 * bucket. No other bucket can hold the item.
 */
[[nodiscard]] inline bool placement_in(const build_data::items::Definition& item,
                                       std::uint8_t bucketId,
                                       ItemPlacement& placement) noexcept {
    if (bucketId == item.bucketId) {
        placement = ItemPlacement::authored;
        return true;
    }
    placement = ItemPlacement::recovery;
    return bucketId == item.recoveryBucketId;
}

} // namespace sunrise::state::account::inventory
