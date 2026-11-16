#pragma once

#include <cstdint>

#include "../../../state/build_data/scriptables/definition.h"
#include "../host_runtime.h"

namespace sunrise::server::activity::mission {

/** Region argument that accepts an occurrence in any region. */
inline constexpr std::int32_t kAnyRegion = -1;

/**
 * Tests whether one authored object's occurrence is resolved and lies in one region.
 * A region numbers a slice-set state the way membership does: slice set plus state ordinal.
 */
[[nodiscard]] inline bool
occurrence_in_region(const state::build_data::scriptables::Snapshot& snapshot,
                     const state::build_data::scriptables::Object& object,
                     std::int32_t region) noexcept {
    const state::build_data::scriptables::State& state = snapshot.states[object.stateRow];
    return state.resolved && state.registryTag == object.registryTag
           && std::uint64_t{state.sliceSetIndex} + state.index
                  == static_cast<std::uint64_t>(region);
}

/**
 * Pending-region changes never establish entry into a held region.
 * @param source Exact accepted client-state after-image.
 * @param output Receives the region transition with the original input ownership.
 * @return True only when the native current-region change flag names a new held region.
 */
[[nodiscard]] inline bool make_region_changed(const host::Event& source,
                                              host::Event& output) noexcept {
    output = {};
    if (source.kind != host::EventKind::clientStateChanged || !source.clientStateHasCurrentRegion
        || source.currentRegionIndex < 0 || source.currentRegionIndex != source.heldRegionIndex
        || source.currentRegionIndex == source.previousRegionIndex) {
        return false;
    }
    output.kind = host::EventKind::regionChanged;
    output.binding = source.binding;
    output.sequence = source.sequence;
    output.tick = source.tick;
    output.attemptGeneration = source.attemptGeneration;
    output.sourceGeneration = source.sourceGeneration;
    output.missionSequence = source.missionSequence;
    output.regionIndex = source.currentRegionIndex;
    output.previousRegionIndex = source.previousRegionIndex;
    return true;
}

} // namespace sunrise::server::activity::mission
