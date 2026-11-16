#pragma once

#include <cstdint>

#include "../../state/activity/bubble_authority/definition.h"

namespace sunrise::server::bap {

/**
 * Releases a link's bubble before a selection reloads it, since a granted bubble places twice.
 * The release repeats the held token and message 33 answers it. Runs under the BAP session lock.
 */
struct BubbleHandoff final {
    /** Requested until the release frame goes out, then published until message 33 answers it. */
    enum class Stage : std::uint8_t { none, requested, published, acknowledged };
    /** ActivityClient generation the release belongs to. */
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
    /**
     * @param heldRegion Region the client is resident in.
     * @return True once nothing is left to release: before the release goes out the client no
     * longer holds the source state, and after it the client is resident in another bubble.
     */
    [[nodiscard]] bool source_left(std::int32_t heldRegion) const noexcept {
        if (stage == Stage::requested) {
            return heldRegion != static_cast<std::int32_t>(source);
        }
        return heldRegion >= 0
               && (static_cast<std::uint32_t>(heldRegion)
                   >> state::activity::bubble_authority::kSliceSetToBubbleShift)
                      != bubble();
    }
    /**
     * Moves the release onto another selection. One in the same bubble keeps the release, which
     * covers the whole bubble; any other has nothing to wait for.
     */
    void retarget(std::uint32_t to) noexcept {
        if ((to >> state::activity::bubble_authority::kSliceSetToBubbleShift) == bubble()
            && to != source) {
            destination = to;
        } else {
            *this = {};
        }
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
