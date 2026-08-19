#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "scenario_reader.h"

namespace sunrise::middleware::content::packages::tables {

/**
 * A destination reaches at most this many slice sets.
 * Slice-set indices are spaced by the slice-set factor across a 512-wide region space, so this
 * is the hard bound. The widest installed destination reaches 47.
 */
inline constexpr std::size_t kSliceSetCapacity = 64;
/**
 * Roster keys tracked for one destination.
 * No installed destination reaches more than 2 objects carrying a wire slot type. This leaves
 * room to spare without a heap allocation.
 */
inline constexpr std::size_t kRosterKeyCapacity = 16;

static_assert(kSliceSetCapacity * kSliceSetIndexFactor == 512);
// One bit per slice set, so the mask must cover the whole capacity.
static_assert(kSliceSetCapacity == 64);

/**
 * Which slice sets each candidate key appears in.
 * A key is safe to send only when it appears in every slice set of the destination. The client
 * dereferences a miss unchecked, so a key missing from one crashes on the switch out of it.
 */
struct RosterIntersection {
    std::array<std::uint32_t, kRosterKeyCapacity> keys{};
    std::array<std::uint64_t, kRosterKeyCapacity> masks{};
    std::size_t keyCount{};
    /** Every slice set observed, whichever key carried it. */
    std::uint64_t observedSets{};
    /** Set when a key or a slice set did not fit, which makes the result unusable. */
    bool overflowed{};
    /** Set when a state's slice set could not be read, which makes every key unsafe. */
    bool unresolvedSet{};
};

/**
 * Records a slice set whose entry could not be read.
 * The destination still moves into that slice set and no key can be proved present in it, so no
 * key of this destination is safe to send afterwards.
 * @param state Accumulator for one destination.
 */
void observe_unresolved_slice_set(RosterIntersection& state) noexcept;

/**
 * The slot types that make an object worth publishing as a roster group.
 * Only 56 installed objects declare any of them, and the key limit above holds only for that
 * filtered set. Feeding every placed object instead overflows most destinations.
 */
inline constexpr std::array<std::uint16_t, 9> kRosterSlotTypes = {
    8, 13, 16, 17, 21, 35, 37, 41, 67};

/**
 * Tests whether one placed object is a roster candidate.
 * @param object Whole placed-object bytes.
 * @return True when it declares a slot of one of the wire types.
 */
[[nodiscard]] bool carries_roster_slot(std::span<const std::byte> object) noexcept;

/**
 * Records a slice set the destination reaches, whether or not it holds a roster object.
 * The intersection is over every slice set of the destination, so a slice set that holds no
 * candidate still has to count. Leaving it out makes an unsafe key look safe.
 * @param state Accumulator for one destination.
 * @param sliceSetIndex Slice-set index as the entry reports it, already scaled by the factor.
 * @return True when the index is inside the region space.
 */
[[nodiscard]] bool observe_slice_set(RosterIntersection& state,
                                     std::uint32_t sliceSetIndex) noexcept;

/**
 * Records that one key appears in one slice set.
 * @param state Accumulator for one destination.
 * @param sliceSetIndex Slice-set index as the entry reports it, already scaled by the factor.
 * @param objectKey Registry key of the placed object.
 * @return True when the observation was recorded.
 */
[[nodiscard]] bool observe_roster_key(RosterIntersection& state,
                                      std::uint32_t sliceSetIndex,
                                      std::uint32_t objectKey) noexcept;

/**
 * Reports the keys present in every observed slice set.
 * @param state Accumulator for one destination.
 * @param output Receives the safe keys.
 * @param count Receives how many were written.
 * @return True when nothing overflowed and every safe key fits the output.
 */
[[nodiscard]] bool safe_roster_keys(const RosterIntersection& state,
                                    std::span<std::uint32_t> output,
                                    std::size_t& count) noexcept;

/**
 * Reports the keys present in some observed slice sets and not all, with the bubbles holding them.
 * These belong in a per-bubble sub-block: the top-level list would make the teardown sweep deref a
 * key the current slice set cannot find. One recorded bit is one bubble.
 * @param state Accumulator for one destination.
 * @param keys Receives the partially present keys.
 * @param masks Receives each key's bubbles, one bit per bubble index, in the same order.
 * @param count Receives how many were written.
 * @return True when nothing overflowed and every partial key fits the output.
 */
[[nodiscard]] bool partial_roster_keys(const RosterIntersection& state,
                                       std::span<std::uint32_t> keys,
                                       std::span<std::uint64_t> masks,
                                       std::size_t& count) noexcept;

} // namespace sunrise::middleware::content::packages::tables
