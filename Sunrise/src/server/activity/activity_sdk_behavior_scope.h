#pragma once

#include <cstdint>
#include <span>

#include "../../middleware/content/packages/tables/scenario_reader.h"
#include "../../state/activity_sdk/format.h"

// A mission seed is published at the opening area and later bubbles keep that lease. Their local
// sequences and scenes belong to the state the live region selects, not the seed state.

namespace sunrise::server::activity::behavior_scope {

namespace format = state::activity_sdk::format;

/** Occurrence ranks: the live region's state wins over the seed state. */
inline constexpr unsigned kRankNone = 0;
inline constexpr unsigned kRankSeedState = 1;
inline constexpr unsigned kRankLiveState = 2;

/** @return True when the state row is the one the client's region packs. */
[[nodiscard]] inline bool live_state(std::span<const format::State> states,
                                     std::span<const format::Bubble> bubbles,
                                     std::uint32_t scenario,
                                     std::uint32_t row,
                                     std::int32_t region) noexcept {
    if (region < 0 || row >= states.size()) {
        return false;
    }
    const format::State& state = states[row];
    if (state.scenarioIndex != scenario || state.bubbleIndex >= bubbles.size()
        || bubbles[state.bubbleIndex].scenarioIndex != scenario
        || state.stateOrdinal >= middleware::content::packages::tables::kSliceSetIndexFactor) {
        return false;
    }
    return static_cast<std::uint64_t>(state.sliceSetIndex) + state.stateOrdinal
           == static_cast<std::uint32_t>(region);
}

struct Selection final {
    std::uint32_t row{format::kAbsentIndex};
    /** Two occurrences of equal rank name different states. */
    bool ambiguous{};
};

/**
 * Selects the occurrence of one object for the live region, or for the seed state when the
 * live region has none.
 */
[[nodiscard]] inline Selection select(std::span<const format::Occurrence> occurrences,
                                      std::span<const format::State> states,
                                      std::span<const format::Bubble> bubbles,
                                      std::uint32_t scenario,
                                      std::uint32_t object,
                                      std::uint32_t seedState,
                                      std::int32_t region) noexcept {
    Selection result{};
    unsigned best = kRankNone;
    for (std::uint32_t index = 0; index < occurrences.size(); ++index) {
        const format::Occurrence& occurrence = occurrences[index];
        if (occurrence.scenarioIndex != scenario || occurrence.objectIndex != object
            || occurrence.stateIndex >= states.size()
            || states[occurrence.stateIndex].scenarioIndex != scenario
            || occurrence.bubbleIndex != states[occurrence.stateIndex].bubbleIndex) {
            continue;
        }
        const unsigned rank = live_state(states, bubbles, scenario, occurrence.stateIndex, region)
                                  ? kRankLiveState
                              : occurrence.stateIndex == seedState ? kRankSeedState
                                                                   : kRankNone;
        if (rank == kRankNone || rank < best) {
            continue;
        }
        if (rank > best) {
            result = {index, false};
            best = rank;
        } else if (occurrences[result.row].stateIndex != occurrence.stateIndex) {
            result.ambiguous = true;
        }
    }
    return result;
}

/** @return The region a state's content lives in: its slice set plus its state ordinal. */
[[nodiscard]] constexpr std::int32_t state_region(const format::State& state) noexcept {
    return static_cast<std::int32_t>(state.sliceSetIndex + state.stateOrdinal);
}

/** @return True when both regions are set and lie in different bubbles. */
[[nodiscard]] constexpr bool other_bubble(std::int32_t region, std::int32_t clientRegion) noexcept {
    namespace tables = middleware::content::packages::tables;
    return region >= 0 && clientRegion >= 0
           && static_cast<std::uint32_t>(region) / tables::kSliceSetIndexFactor
                  != static_cast<std::uint32_t>(clientRegion) / tables::kSliceSetIndexFactor;
}

/**
 * Selects the occurrence of an object outside the client's bubble, so a write goes to its state.
 * Occurrences in two states give an ambiguous result that names the first.
 */
[[nodiscard]] inline Selection select_unheld(std::span<const format::Occurrence> occurrences,
                                             std::span<const format::State> states,
                                             std::uint32_t scenario,
                                             std::uint32_t object,
                                             std::int32_t clientRegion) noexcept {
    namespace tables = middleware::content::packages::tables;
    Selection result{};
    for (std::uint32_t index = 0; index < occurrences.size(); ++index) {
        const format::Occurrence& occurrence = occurrences[index];
        if (occurrence.scenarioIndex != scenario || occurrence.objectIndex != object
            || occurrence.stateIndex >= states.size()
            || states[occurrence.stateIndex].scenarioIndex != scenario
            || occurrence.bubbleIndex != states[occurrence.stateIndex].bubbleIndex
            || states[occurrence.stateIndex].stateOrdinal >= tables::kSliceSetIndexFactor) {
            continue;
        }
        const format::State& state = states[occurrence.stateIndex];
        if (!other_bubble(state_region(state), clientRegion)) {
            return {};
        }
        if (result.row == format::kAbsentIndex) {
            result.row = index;
        } else if (occurrences[result.row].stateIndex != occurrence.stateIndex) {
            result.ambiguous = true;
        }
    }
    return result;
}

} // namespace sunrise::server::activity::behavior_scope
