#pragma once

#include <cstdint>

#include "../../../../../state/activity/membership/definition.h"

namespace sunrise::server::bap::encrypted::push::activity {

/** What one roster body knows about its client's placement when it picks a bubble to grant. */
struct GrantPlacement final {
    /** Other authenticated links bind this activity session, so they share its grant table. */
    bool sharedBubbles{};
    /** The client holds a slice set and no host move is due. */
    bool regionReady{};
    /** Region of the slice set the client holds, negative while it holds none. */
    std::int32_t heldRegion{state::activity::membership::kAbsentRegionIndex};
    /** The bubble the client holds changed with this body. */
    bool enteringBubble{};
    /** Region the body publishes. */
    std::int32_t bodyRegion{state::activity::membership::kAbsentRegionIndex};
    /** Pending leg the client last committed. */
    std::int32_t pendingRegion{state::activity::membership::kAbsentRegionIndex};
};

/** The region whose bubble a body may grant, and whether a released grant may be renewed. */
struct GrantRequest final {
    std::int32_t region{state::activity::membership::kAbsentRegionIndex};
    bool renewReleased{};
};

/**
 * Picks the region a roster body may grant bubble authority for.
 * Links that share a session share one grant per bubble, and the holder's client runs that
 * bubble's simulation. So the bubble goes to the first client whose report says it holds it: a
 * client still heading in, or the region behind a z-leg switch that the switch report's pending leg
 * names, would take it from a client already inside. The holder keeps it until it abdicates, and a
 * client still inside takes it then.
 * A sole link is also the first client to cross, so its grant stays on the heading leg and is in
 * place before the slice-set switch.
 * @param placement The body's view of its client.
 * @return The region, negative when none may be granted, and whether a released grant renews.
 */
[[nodiscard]] constexpr GrantRequest grant_request(const GrantPlacement& placement) noexcept {
    if (placement.sharedBubbles) {
        return {placement.regionReady ? placement.heldRegion
                                      : state::activity::membership::kAbsentRegionIndex,
                placement.regionReady};
    }
    // Authority is granted for the bubble the client is heading to, so it is in place before the
    // slice-set switch, and for the one it is in otherwise.
    const std::int32_t region = placement.enteringBubble       ? placement.bodyRegion
                                : placement.pendingRegion >= 0 ? placement.pendingRegion
                                                               : placement.bodyRegion;
    return {region, placement.enteringBubble || placement.regionReady};
}

} // namespace sunrise::server::bap::encrypted::push::activity
