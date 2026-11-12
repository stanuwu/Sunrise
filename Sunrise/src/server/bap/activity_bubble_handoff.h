#pragma once

#include <cstdint>

#include "../../state/activity/bubble_authority/definition.h"

namespace sunrise::server::bap {

/**
 * Releases one link's bubble before a state selection replaces the world inside that bubble.
 * The native load pass creates replicated placements whenever the bubble is granted, and only
 * the seed pass skips placements that already exist, so the client must give the bubble up
 * before it loads the sibling state. The release reuses the held token; the client answers with
 * message 33, and the destination's renewal then reclaims what it released. Every step runs
 * under the BAP session lock.
 */
struct BubbleHandoff final {
    enum class Stage : std::uint8_t { none, requested, published, acknowledged };
    std::uint64_t generation{};
    /** Effective regions of the state the client holds and of the selected state. */
    std::uint32_t source{};
    std::uint32_t destination{};
    /** Token the bubble is held under, which the release repeats. */
    std::uint16_t token{};
    Stage stage{};

    /** @return The bubble both regions belong to. */
    [[nodiscard]] std::uint8_t bubble() const noexcept {
        return static_cast<std::uint8_t>(
            source >> state::activity::bubble_authority::kSliceSetToBubbleShift);
    }
    /** @return True while this handoff belongs to the link's current client binding. */
    [[nodiscard]] bool active(std::uint64_t current) const noexcept {
        return stage != Stage::none && generation == current;
    }
    /** @return True when this handoff is the exact pending move. */
    [[nodiscard]] bool
    matches(std::uint64_t current, std::uint32_t from, std::uint32_t to) const noexcept {
        return active(current) && source == from && destination == to;
    }
    /** Advances once the release frame reaches the caller. */
    bool publish(std::uint64_t current,
                 std::uint8_t releasedBubble,
                 std::uint16_t releasedToken) noexcept {
        if (!active(current) || stage != Stage::requested || bubble() != releasedBubble
            || token != releasedToken) {
            return false;
        }
        stage = Stage::published;
        return true;
    }
    /** Advances on the client's message 33 for the released bubble, only after the release. */
    bool acknowledge(std::uint64_t current, std::uint8_t abdicatedBubble) noexcept {
        if (!active(current) || stage != Stage::published || bubble() != abdicatedBubble) {
            return false;
        }
        stage = Stage::acknowledged;
        return true;
    }
};

} // namespace sunrise::server::bap
