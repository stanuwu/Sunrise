#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "../definition.h"

namespace sunrise::state::matchmaking::transactions {

/** @return The value that means a variant is not in the context table. */
inline constexpr std::size_t no_variant() noexcept {
    return kVariantCapacity;
}

/**
 * Finds the slot for an active handle while the caller owns the State lock.
 * @param state Matchmaking State protected by the lock.
 * @param handle Generation-checked context handle.
 * @return Matching active slot, or null when the handle is stale.
 */
inline ContextSlot* resolve(MatchmakingState& state, ContextHandle handle) noexcept {
    if (handle.generation == kInvalidGeneration || handle.slot >= state.contexts.size()) {
        return nullptr;
    }
    ContextSlot& slot = state.contexts[handle.slot];
    return slot.active && slot.generation == handle.generation ? &slot : nullptr;
}

/**
 * Finds a variant in one context.
 * @param data Context table to search.
 * @param variant Variant key to match.
 * @return Matching index, or the table-capacity sentinel.
 */
inline std::size_t find_variant(const ContextData& data, std::uint64_t variant) noexcept {
    for (std::size_t index = 0; index < data.variants.size(); ++index) {
        const VariantRecord& record = data.variants[index];
        if (record.occupied && record.variant == variant) {
            return index;
        }
    }
    return no_variant();
}

/**
 * Picks fixed storage for a new variant.
 * @param data Context table to inspect.
 * @return First empty slot, otherwise the least-recently-updated slot.
 */
inline std::size_t select_variant_slot(const ContextData& data) noexcept {
    for (std::size_t index = 0; index < data.variants.size(); ++index) {
        if (!data.variants[index].occupied) {
            return index;
        }
    }
    std::size_t selected = 0;
    for (std::size_t index = 1; index < data.variants.size(); ++index) {
        // A strict compare makes equal revisions evict the lowest table index.
        if (data.variants[index].lastUpdatedRevision
            < data.variants[selected].lastUpdatedRevision) {
            selected = index;
        }
    }
    return selected;
}

/**
 * Checks the rising allocator can commit one more id.
 * @param state Matchmaking State to inspect.
 * @return True when both the id and the revision can still be stored.
 */
inline bool allocator_available(const MatchmakingState& state) noexcept {
    return !state.allocatorExhausted && state.nextAdvertisementId != kAbsentAdvertisementId
           && state.allocatorRevision != (std::numeric_limits<std::uint64_t>::max)();
}

/**
 * Advances the nonzero allocator after every commit check has passed.
 * @param state Matchmaking State to update.
 */
inline void advance_allocator(MatchmakingState& state) noexcept {
    if (state.nextAdvertisementId == (std::numeric_limits<std::uint64_t>::max)()) {
        state.allocatorExhausted = true;
    } else {
        ++state.nextAdvertisementId;
    }
    ++state.allocatorRevision;
}

/**
 * Checks the descriptor ownership fields before taking the State lock.
 * @param mutation Prepared mutation supplied by the caller.
 * @return True when the short-lived descriptor view agrees with itself.
 */
inline bool valid_descriptor(const PendingMutation& mutation) noexcept {
    if (mutation.hasDescriptor) {
        return mutation.descriptorData != nullptr && mutation.descriptorSize == kDescriptorSize;
    }
    return mutation.descriptorData == nullptr && mutation.descriptorSize == 0;
}

} // namespace sunrise::state::matchmaking::transactions
