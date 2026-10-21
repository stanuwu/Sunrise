#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "../../../state/activity/bubble_authority/definition.h"
#include "../../../state/build_data/scenarios/definition.h"
#include "../../bap/runtime.h"
#include "../host_runtime.h"

namespace sunrise::server::activity::mission {

/** The wire ClientRef one output names: its object's registry key and one slot inside it. */
struct OutputClientRef final {
    std::uint32_t registryKey{};
    std::uint16_t slotIndex{};
    std::uint8_t slotType{};
};

/** One bit per authored bubble ordinal. */
using BubbleMask = std::uint64_t;
static_assert(state::build_data::scenarios::kBubbleCapacity <= sizeof(BubbleMask) * 8U);

/**
 * A canonical body keeps its table's object tag, so the tag is not part of the match.
 * @return True when a retained body's target is this ClientRef.
 */
[[nodiscard]] inline bool same_client_ref(const host::ScriptablePin& pin,
                                          const OutputClientRef& ref) noexcept {
    return pin.registryKey == ref.registryKey && pin.slotIndex == ref.slotIndex
           && pin.slotType == ref.slotType;
}

/**
 * Lists the links an output must follow because a retained body already lives there. A target's
 * reports are admitted only from the link its body was staged on, so an output that drives or
 * watches a placed target lands on that link: the target's own body first, then its parent squad's
 * body, then any squad body of the target's object.
 * @param estate Pins of the binding's retained Auth bodies.
 * @param ref Client identity the output names.
 * @param parentSquad The squad an actor output's member came from, when the intent names it.
 * @param sameObjectSquads Also follows squad bodies of the target's object.
 * @param output Receives distinct nonzero pinned generations in that order.
 * @return The number of generations written, at most output.size().
 */
[[nodiscard]] inline std::size_t retained_pins(std::span<const host::ScriptablePin> estate,
                                               const OutputClientRef& ref,
                                               const std::optional<OutputClientRef>& parentSquad,
                                               bool sameObjectSquads,
                                               std::span<std::uint64_t> output) noexcept {
    std::size_t count = 0;
    const auto add = [&](std::uint64_t generation) noexcept {
        const auto written = output.begin() + static_cast<std::ptrdiff_t>(count);
        if (generation != 0 && count < output.size()
            && std::find(output.begin(), written, generation) == written) {
            output[count++] = generation;
        }
    };
    const auto add_exact = [&](const OutputClientRef& wanted) noexcept {
        for (const host::ScriptablePin& retained : estate) {
            if (same_client_ref(retained, wanted)) {
                add(retained.expectedActivityClientGeneration);
            }
        }
    };
    add_exact(ref);
    if (parentSquad.has_value()) {
        add_exact(*parentSquad);
    }
    if (sameObjectSquads) {
        for (const host::ScriptablePin& retained : estate) {
            if (retained.kind == host::ScriptableOverrideKind::squad
                && retained.registryKey == ref.registryKey) {
                add(retained.expectedActivityClientGeneration);
            }
        }
    }
    return count;
}

/**
 * Who simulates one target, for every report the program reads about it. The retail host applies
 * an output in the bubble that owns its target and the client holding that bubble simulates it,
 * so this follows the chain `retained_pins` routes outputs by: the target's own body, else a squad
 * body of its object, else the route. A pin counts only while it names a live link, because a body
 * pinned to a link that left or whose generation advanced no longer speaks for its target.
 * @param estate Pins of the binding's retained Auth bodies.
 * @param ref Client identity the report names.
 * @param route The program's route, which simulates every target no live pin claims.
 * @param live The route and every joined private link of the binding.
 * @return The one link whose reports for this target are admitted.
 */
[[nodiscard]] inline std::uint64_t
admitted_generation(std::span<const host::ScriptablePin> estate,
                    const OutputClientRef& ref,
                    std::uint64_t route,
                    std::span<const std::uint64_t> live) noexcept {
    const auto counts = [live](const host::ScriptablePin& pin) noexcept {
        return pin.expectedActivityClientGeneration != 0
               && std::find(live.begin(), live.end(), pin.expectedActivityClientGeneration)
                      != live.end();
    };
    for (const host::ScriptablePin& pin : estate) {
        if (same_client_ref(pin, ref) && counts(pin)) {
            return pin.expectedActivityClientGeneration;
        }
    }
    for (const host::ScriptablePin& pin : estate) {
        if (pin.kind == host::ScriptableOverrideKind::squad && pin.registryKey == ref.registryKey
            && counts(pin)) {
            return pin.expectedActivityClientGeneration;
        }
    }
    return route;
}

/** @return The bit of one bubble ordinal, or none past the bubble capacity. */
[[nodiscard]] inline BubbleMask bubble_bit(std::uint32_t bubble) noexcept {
    return bubble < state::build_data::scenarios::kBubbleCapacity ? BubbleMask{1} << bubble
                                                                  : BubbleMask{};
}

/** @return The bubble of the region one link's client is in, or none without a region. */
[[nodiscard]] inline BubbleMask link_bubble(const server::bap::ActivityLinkView& link) noexcept {
    namespace tables = middleware::content::packages::tables;
    return link.effectiveRegion >= 0 ? bubble_bit(static_cast<std::uint32_t>(link.effectiveRegion)
                                                  / tables::kSliceSetIndexFactor)
                                     : BubbleMask{};
}

/**
 * A grant holder can move a pin only once two links are live: with one, every live holder is that
 * link, and a pin naming no live link already falls back to it.
 * @param liveLinks The route and every joined private link of the binding.
 */
[[nodiscard]] constexpr bool holders_route_pins(std::size_t liveLinks) noexcept {
    return liveLinks >= state::activity::bubble_authority::kSharedLinkCount;
}

/**
 * The link one retained body's target is simulated on. The client holding a bubble's grant runs
 * that bubble's simulation, so a target of exactly one bubble follows that bubble's live holder,
 * whichever link its body was staged on. A target of several bubbles, or of a bubble nobody
 * holds, keeps its pin.
 * @param pinned Generation the body was staged on.
 * @param bubbles The target's bubbles.
 * @param holders Each bubble's holder generation, zero for none.
 * @param live The route and every joined private link of the binding.
 * @return The generation the pin names from now on.
 */
[[nodiscard]] inline std::uint64_t grant_holder_pin(std::uint64_t pinned,
                                                    BubbleMask bubbles,
                                                    std::span<const std::uint64_t> holders,
                                                    std::span<const std::uint64_t> live) noexcept {
    if (std::popcount(bubbles) != 1) {
        return pinned;
    }
    const auto bubble = static_cast<std::size_t>(std::countr_zero(bubbles));
    const std::uint64_t holder = bubble < holders.size() ? holders[bubble] : 0;
    return holder != 0 && std::find(live.begin(), live.end(), holder) != live.end() ? holder
                                                                                    : pinned;
}

/** @return True when one link holds the grant of one of the target's bubbles. */
[[nodiscard]] inline bool holds_target_grant(const server::bap::ActivityLinkView& link,
                                             BubbleMask bubbles,
                                             std::span<const std::uint64_t> holders) noexcept {
    for (std::size_t bubble = 0; bubble < holders.size(); ++bubble) {
        if ((bubbles & bubble_bit(static_cast<std::uint32_t>(bubble))) != 0 && holders[bubble] != 0
            && holders[bubble] == link.activityClientGeneration) {
            return true;
        }
    }
    return false;
}

/**
 * Moves the links that hold one of the target's bubbles ahead of the route, after the pinned
 * links. The link holding the bubble's grant comes first, because its client is the one that
 * simulates the bubble and so the only one whose reports for the target are admitted. Then a link
 * counts by the region its client reports. A link known only by its arrival slice set counts for a
 * bubble no reporting link holds, since its client may still be loading there. Every group keeps
 * its order, so the route stays first among the reporting holders.
 * @param candidates Joined links, pinned first, then the route, then the rest.
 * @param pins Pinned generations; the leading candidates among them stay in place.
 * @param bubbles The target's bubbles; none leaves the order unchanged.
 * @param holders Each bubble's grant holder generation, zero for none.
 */
inline void promote_bubble_holders(std::span<server::bap::ActivityLinkView> candidates,
                                   std::span<const std::uint64_t> pins,
                                   BubbleMask bubbles,
                                   std::span<const std::uint64_t> holders) noexcept {
    std::size_t next = 0;
    while (next < candidates.size()
           && std::find(pins.begin(), pins.end(), candidates[next].activityClientGeneration)
                  != pins.end()) {
        ++next;
    }
    for (std::size_t index = next; index < candidates.size(); ++index) {
        if (holds_target_grant(candidates[index], bubbles, holders)) {
            std::rotate(candidates.begin() + static_cast<std::ptrdiff_t>(next),
                        candidates.begin() + static_cast<std::ptrdiff_t>(index),
                        candidates.begin() + static_cast<std::ptrdiff_t>(index + 1));
            ++next;
        }
    }
    BubbleMask reported{};
    for (const server::bap::ActivityLinkView& link : candidates) {
        if (link.effectiveRegionReported) {
            reported |= link_bubble(link) & bubbles;
        }
    }
    for (std::size_t index = next; index < candidates.size(); ++index) {
        const BubbleMask held = link_bubble(candidates[index]) & bubbles;
        if (held != 0 && (candidates[index].effectiveRegionReported || (held & reported) == 0)) {
            std::rotate(candidates.begin() + static_cast<std::ptrdiff_t>(next),
                        candidates.begin() + static_cast<std::ptrdiff_t>(index),
                        candidates.begin() + static_cast<std::ptrdiff_t>(index + 1));
            ++next;
        }
    }
}

/** How one adapter family's statuses steer a walk across links. */
template <typename Result> struct WalkPolicy final {
    Result queued{};
    /** Statuses of a link that lacks the target, its region or its generation: try the next. */
    std::span<const Result> next{};
    /** A status that waits on its link. It is kept, and returned unless a later link queues. */
    std::optional<Result> wait{};
};

/**
 * Runs one adapter on each candidate in turn until one queues or refuses for good. The route runs
 * last when no candidate is the route, so the program can always deliver on its own route.
 * @param route The program's route.
 * @param carrier Receives the generation of the link the returned status came from.
 * @param adapter Runs the family's reserved request for one generation.
 * @return The status that ended the walk, or the kept wait status.
 */
template <typename Result, typename Adapter>
[[nodiscard]] Result walk_links(std::span<const server::bap::ActivityLinkView> candidates,
                                std::uint64_t route,
                                const WalkPolicy<Result>& policy,
                                std::uint64_t& carrier,
                                const Adapter& adapter) noexcept {
    const bool routeListed = std::any_of(
        candidates.begin(), candidates.end(), [route](const server::bap::ActivityLinkView& link) {
            return link.activityClientGeneration == route;
        });
    const std::size_t attempts = candidates.size() + (routeListed ? 0U : 1U);
    std::optional<Result> waiting{};
    std::uint64_t waitingCarrier = 0;
    Result status{};
    for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
        carrier =
            attempt < candidates.size() ? candidates[attempt].activityClientGeneration : route;
        status = adapter(carrier);
        if (status == policy.queued) {
            return status;
        }
        if (policy.wait.has_value() && status == *policy.wait) {
            if (!waiting.has_value()) {
                waiting = status;
                waitingCarrier = carrier;
            }
            continue;
        }
        if (std::find(policy.next.begin(), policy.next.end(), status) == policy.next.end()) {
            break;
        }
    }
    if (waiting.has_value()) {
        carrier = waitingCarrier;
        return *waiting;
    }
    return status;
}

} // namespace sunrise::server::activity::mission
