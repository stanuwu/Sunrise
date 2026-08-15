#include "../../items/details/item_detail_catalog.h"
#include "validation.h"

namespace sunrise::state::build_data::cache::records {

/**
 * Checks every configured detail reference against the other numeric domains.
 * @param details Item details that already passed their own checks.
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
    // The domain holds equipped items and the plugs they socket. Only the equipped rows can be
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

} // namespace sunrise::state::build_data::cache::records
