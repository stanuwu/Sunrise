#pragma once

#include <cstdint>

#include "../../../../middleware/bap/activity_message/activity_patch_epoch_parser.h"
#include "../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../state/activity/runtime.h"

namespace sunrise::server::bap::encrypted::activity_message {

/** Outbound delivery staged for one activity State transaction. */
enum class Delivery : std::uint8_t {
    none,
    joinNotifications,
    entitySlotNotification,
    membershipNotification,
    /**
     * The client's own state-refresh request. It asks for the host snapshot, not one message, so
     * the answer is the global state, the membership and the roster in that order -- the same three
     * the keepalive carries.
     */
    refreshNotifications,
    /**
     * One client-authoritative delta. It publishes membership when the host state changed and the
     * roster when the player moved region, and either, both or neither may apply.
     */
    authoritativeNotifications,
};

/** State transaction family staged by one activity service request. */
enum class MutationDomain : std::uint8_t {
    none,
    entitySlots,
    membership,
    /** The patch epoch is kept on the connection and changes no State. */
    patchEpoch,
};

/** Scalar and mask data kept after the sensitive svc8 payload view expires. */
struct ActivityPlan final {
    std::uint32_t correlation{};
    std::uint64_t sessionId{};
    state::activity::entity_slots::PendingMutation entitySlotMutation{};
    state::activity::membership::PendingMutation membershipMutation{};
    middleware::bap::activity_message::patch_epoch::PatchEpoch patchEpoch{};
    /** The character the join request named, or zero when it carried none. */
    std::uint64_t joinCharacterSoid{};
    /**
     * Set when the delta moved the player to a different region.
     * The new region has no bubble authority until the roster grants it, so waiting for the next
     * roster tick leaves it empty for up to a whole interval.
     */
    bool regionMoved{};
    /**
     * Set when the delta moved the client's transition token, which means it is starting a load.
     * It is the only start signal on the wire, and the roster's faster cadence runs off it.
     */
    bool transitionStarted{};
    Delivery delivery{};
    MutationDomain mutationDomain{};
};

} // namespace sunrise::server::bap::encrypted::activity_message
