#include "inventory_bucket_catalog.h"

#include <algorithm>
#include <array>
#include <limits>

#include "../../table.h"

namespace sunrise::state::build_data::inventory::buckets {
namespace {

/** An all-one row marks a bucket id with no published descriptor. */
constexpr std::uint16_t kEmptyLookupRow = (std::numeric_limits<std::uint16_t>::max)();

Lock g_lock;
Table<Descriptor, kDescriptorCapacity> g_descriptors;
// Bucket id to descriptor row, rebuilt with the table under the same exclusive hold.
std::array<std::uint16_t, kDescriptorCapacity> g_lookup{};

/**
 * Gets the fixed inventory size one descriptor picks.
 * @param selector Native inventory-array selector.
 * @param capacity Receives the picked array's fixed size.
 * @return True when the selector names an array we support.
 */
[[nodiscard]] bool selector_capacity(ArraySelector selector, std::uint32_t& capacity) noexcept {
    switch (selector) {
    case ArraySelector::character:
        capacity = kCharacterSlotCapacity;
        return true;
    case ArraySelector::profile:
        capacity = kProfileSlotCapacity;
        return true;
    case ArraySelector::smallProfile:
        capacity = kSmallProfileSlotCapacity;
        return true;
    }
    capacity = 0;
    return false;
}

/** @return True when the row's slot range fits its picked array. */
[[nodiscard]] bool range_fits(const Descriptor& descriptor) noexcept {
    std::uint32_t capacity = 0;
    if (descriptor.slotCount == 0 || !selector_capacity(descriptor.arraySelector, capacity)) {
        return false;
    }
    const std::uint32_t firstSlot = descriptor.firstSlot;
    return firstSlot <= capacity && descriptor.slotCount <= capacity - firstSlot;
}

} // namespace

/** Clears every generated inventory-bucket descriptor under the catalog lock. */
void clear() noexcept {
    const Lock::Exclusive guard(g_lock);
    g_descriptors.clear();
    std::fill(g_lookup.begin(), g_lookup.end(), kEmptyLookupRow);
}

/** Checks that bucket ids are unique, selectors are known, and slot ranges fit. */
bool valid(std::span<const Descriptor> descriptors) noexcept {
    if (descriptors.empty() || descriptors.size() > kDescriptorCapacity) {
        return false;
    }

    std::array<bool, kDescriptorCapacity> occupied{};
    for (const Descriptor& descriptor : descriptors) {
        if (descriptor.bucketId == kUnavailableBucketId || occupied[descriptor.bucketId]
            || !range_fits(descriptor)) {
            return false;
        }
        occupied[descriptor.bucketId] = true;
    }
    return true;
}

/** Rebuilds the descriptor table and its bucket-id lookup after the checks pass. */
bool replace(std::span<const Descriptor> descriptors) noexcept {
    if (!valid(descriptors)) {
        return false;
    }

    const Lock::Exclusive guard(g_lock);
    std::fill(g_lookup.begin(), g_lookup.end(), kEmptyLookupRow);
    if (!g_descriptors.replace(descriptors)) {
        return false;
    }
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
        g_lookup[descriptors[index].bucketId] = static_cast<std::uint16_t>(index);
    }
    return true;
}

/** Finds one inventory-bucket routing descriptor by native bucket id. */
bool find(std::uint8_t bucketId, Descriptor& descriptor) noexcept {
    descriptor = {};
    if (bucketId == kUnavailableBucketId) {
        return false;
    }

    const Lock::Shared guard(g_lock);
    const std::span<const Descriptor> rows = g_descriptors.rows();
    const std::uint16_t row = g_lookup[bucketId];
    const bool found = row != kEmptyLookupRow && row < rows.size();
    if (found) {
        descriptor = rows[row];
    }
    return found;
}

/** Copies descriptors in publication order, without exposing the catalog storage. */
bool snapshot(std::span<Descriptor> output, std::size_t& count) noexcept {
    const Lock::Shared guard(g_lock);
    return g_descriptors.snapshot(output, count);
}

/** @return Number of inventory-bucket descriptors, read under the lock. */
std::size_t count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_descriptors.count();
}

} // namespace sunrise::state::build_data::inventory::buckets
