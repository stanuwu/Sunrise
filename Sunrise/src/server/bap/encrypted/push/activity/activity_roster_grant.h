#pragma once

#include <cstddef>
#include <cstdint>

#include "../../../../../middleware/bap/activity_message/entity_slots.h"
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

/**
 * Epoch steps a message-24 claim adds to the epoch its client holds. The native applier
 * (0x170CFB0) bumps its epoch only when the claim's differs, and only by one, so a claim at the
 * held epoch adopts without moving the session's replication sequence. Which of the two retail
 * sends is not decoded. A step of one would also have to advance that sequence, as the retirement
 * purge does, so zero is the only value implemented.
 */
inline constexpr std::uint8_t kClaimAuthorityEpochStep = 0;
/** The claim's reason; the native applier forwards it to adoption, and the purge sends zero. */
inline constexpr std::int8_t kClaimAuthorityReason = 0;

/** What a body granting a shared bubble knows about the bubble's previous holder. */
struct HandoverClaimInput final {
    /** Other authenticated links bind this private activity session. */
    bool sharedBubbles{};
    /** The body grants `bubble` to this link. */
    bool granted{};
    std::uint8_t bubble{};
    /** ActivityClient generation receiving the grant. */
    std::uint64_t recipient{};
    /** Generation whose abdication last released the bubble, zero when none did. */
    std::uint64_t releasedBy{};
    /** Entities that abdication released and no claim has delivered yet. */
    const middleware::bap::activity_message::entity_slots::EntitySlotMask* released{};
    /** Entities a purge earlier in the same body frees, or null when it carries none. */
    const middleware::bap::activity_message::entity_slots::EntitySlotMask* purged{};
    /** Replication epoch the client holds before this body. */
    std::uint8_t heldEpoch{};
};

/** One staged message-24 claim. */
struct HandoverClaim final {
    middleware::bap::activity_message::entity_slots::EntitySlotMask slots{};
    std::uint64_t releasedBy{};
    std::uint8_t bubble{};
    /** Epoch the client holds before this body; the claim is stale once the link moves past it. */
    std::uint8_t heldEpoch{};
    std::uint8_t epoch{};
    std::size_t slotCount{};
    bool pending{};
};

/**
 * A client granted a bubble another link released adopts the entities that link simulated there,
 * instead of leaving them frozen as replicas nobody runs. A link renewing its own release, a sole
 * link and a body without a grant claim nothing.
 * @param input The grant body's view of the bubble.
 * @return The claim, with `pending` false when none is owed.
 */
[[nodiscard]] constexpr HandoverClaim handover_claim(const HandoverClaimInput& input) noexcept {
    HandoverClaim claim{};
    if (!input.sharedBubbles || !input.granted || input.released == nullptr || input.releasedBy == 0
        || input.releasedBy == input.recipient) {
        return claim;
    }
    for (std::size_t index = 0; index < claim.slots.size(); ++index) {
        auto bits = (*input.released)[index];
        if (input.purged != nullptr) {
            bits &= ~(*input.purged)[index];
        }
        claim.slots[index] = bits;
        for (auto value = std::to_integer<unsigned>(bits); value != 0; value &= value - 1U) {
            ++claim.slotCount;
        }
    }
    if (claim.slotCount == 0) {
        claim.slots = {};
        return claim;
    }
    claim.releasedBy = input.releasedBy;
    claim.bubble = input.bubble;
    claim.heldEpoch = input.heldEpoch;
    // A purge ahead of the claim in the same body steps the client's epoch first.
    claim.epoch = static_cast<std::uint8_t>(input.heldEpoch + (input.purged != nullptr ? 1U : 0U)
                                            + kClaimAuthorityEpochStep);
    claim.pending = true;
    return claim;
}

} // namespace sunrise::server::bap::encrypted::push::activity
