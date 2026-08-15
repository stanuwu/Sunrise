#include "roster_intersection.h"

namespace sunrise::middleware::content::packages::tables {
namespace {

/** The widest slice-set index the region space allows. */
constexpr std::uint32_t kSliceSetIndexBound = 512;

/**
 * Turns a slice-set index into its bit position.
 * @param sliceSetIndex Index as the entry reports it, already scaled by the factor.
 * @param bit Receives the position.
 * @return True when the index is inside the region space and on the factor's spacing.
 */
[[nodiscard]] bool slice_set_bit(std::uint32_t sliceSetIndex, std::uint32_t& bit) noexcept {
    bit = 0;
    if (sliceSetIndex >= kSliceSetIndexBound || sliceSetIndex % kSliceSetIndexFactor != 0) {
        return false;
    }
    bit = sliceSetIndex / kSliceSetIndexFactor;
    return true;
}

} // namespace

/**
 * Tests whether one placed object is a roster candidate.
 * @param object Whole placed-object bytes.
 * @return True when it declares a slot of one of the wire types.
 */
bool carries_roster_slot(std::span<const std::byte> object) noexcept {
    Array slots{};
    if (!object_slots(object, slots)) {
        return false;
    }
    for (std::uint64_t index = 0; index < slots.count; ++index) {
        Slot slot{};
        if (!object_slot_at(object, slots, index, slot)) {
            return false;
        }
        for (const std::uint16_t wanted : kRosterSlotTypes) {
            if (slot.type == wanted) {
                return true;
            }
        }
    }
    return false;
}

/**
 * Records a slice set the destination reaches, whether or not it holds a roster object.
 * @param state Accumulator for one destination.
 * @param sliceSetIndex Slice-set index as the entry reports it, already scaled by the factor.
 * @return True when the index is inside the region space.
 */
bool observe_slice_set(RosterIntersection& state, std::uint32_t sliceSetIndex) noexcept {
    std::uint32_t bit = 0;
    if (!slice_set_bit(sliceSetIndex, bit)) {
        state.overflowed = true;
        return false;
    }
    state.observedSets |= std::uint64_t{1} << bit;
    return true;
}

/**
 * Records that one key appears in one slice set.
 * @param state Accumulator for one destination.
 * @param sliceSetIndex Slice-set index as the entry reports it, already scaled by the factor.
 * @param objectKey Registry key of the placed object.
 * @return True when the observation was recorded.
 */
bool observe_roster_key(RosterIntersection& state,
                        std::uint32_t sliceSetIndex,
                        std::uint32_t objectKey) noexcept {
    std::uint32_t bit = 0;
    if (!slice_set_bit(sliceSetIndex, bit)) {
        state.overflowed = true;
        return false;
    }
    const std::uint64_t mask = std::uint64_t{1} << bit;
    state.observedSets |= mask;

    for (std::size_t index = 0; index < state.keyCount; ++index) {
        if (state.keys[index] == objectKey) {
            state.masks[index] |= mask;
            return true;
        }
    }
    if (state.keyCount == kRosterKeyCapacity) {
        // A dropped key would read as absent from every slice set, so the whole result is unusable.
        state.overflowed = true;
        return false;
    }
    state.keys[state.keyCount] = objectKey;
    state.masks[state.keyCount] = mask;
    ++state.keyCount;
    return true;
}

/**
 * Records a slice set whose entry could not be read.
 * @param state Accumulator for one destination.
 */
void observe_unresolved_slice_set(RosterIntersection& state) noexcept {
    state.unresolvedSet = true;
}

/**
 * Reports the keys present in every observed slice set.
 * @param state Accumulator for one destination.
 * @param output Receives the safe keys.
 * @param count Receives how many were written.
 * @return True when nothing overflowed and every safe key fits the output.
 */
bool safe_roster_keys(const RosterIntersection& state,
                      std::span<std::uint32_t> output,
                      std::size_t& count) noexcept {
    count = 0;
    if (state.overflowed) {
        return false;
    }
    // An unread slice set proves no key, so the intersection over all of them is empty.
    if (state.unresolvedSet) {
        return true;
    }
    for (std::size_t index = 0; index < state.keyCount; ++index) {
        if (state.masks[index] != state.observedSets) {
            continue;
        }
        if (count == output.size()) {
            count = 0;
            return false;
        }
        output[count] = state.keys[index];
        ++count;
    }
    return true;
}

} // namespace sunrise::middleware::content::packages::tables
