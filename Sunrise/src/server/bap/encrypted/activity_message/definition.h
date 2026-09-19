#pragma once

#include <cstdint>

#include "../../../../middleware/bap/activity_message/activity_host_control.h"
#include "../../../../middleware/bap/activity_message/activity_patch_epoch_parser.h"
#include "../../../../middleware/bap/activity_message/entity_authority.h"
#include "../../../../middleware/bap/activity_message/transport_report.h"
#include "../../../../state/activity/member_departure.h"
#include "../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../state/activity/runtime.h"
#include "../../../gameplay/group/group_host_sessions.h"

namespace sunrise::server::bap::encrypted::activity_message {

/** Outbound delivery staged for one activity State transaction. */
enum class Delivery : std::uint8_t {
    none,
    joinNotifications,
    entitySlotNotification,
    membershipNotification,
    /**
     * The client's own state-refresh request. It asks for the host snapshot, not one message.
     * So the answer is the global state, the membership and the roster, in that order. Those
     * are the same three the keepalive carries.
     */
    refreshNotifications,
    /**
     * One client-reported, host-committed state delta. It publishes membership when the retained
     * host state changed and the roster when the player moved region; either, both or neither may
     * apply.
     */
    authoritativeNotifications,
    /** The roster, answering the patch epoch its body has to echo back. */
    rosterNotification,
    purgeNotification,
    /**
     * The leave delta answering activity msg 15. It keeps every roster key and clears its
     * presence, which is what unregisters the rows that client holds while their owner is valid.
     */
    leaveNotification,
};

/** State transaction family staged by one activity service request. */
enum class MutationDomain : std::uint8_t {
    none,
    entitySlots,
    membership,
    reservations,
    /** The patch epoch is kept on the connection and changes no State. */
    patchEpoch,
    /** Query answers are retained only by their exact connection generation. */
    authorityQuery,
    /** Reset acknowledgements are retained only by their exact connection generation. */
    authorityReset,
    authorityAbdication,
    authorityPurge,
};

/** Connection binding change staged by an activity join. */
enum class BindingIntent : std::uint8_t {
    none,
    preserveCurrent,
    publicTarget,
    sharedTarget,
};

/** Join ingress metadata retained until its binding transaction commits. */
struct JoinIngressDiagnostic final {
    std::uint64_t payloadFingerprint{};
    std::uint32_t payloadBytes{};
    std::uint32_t peerHeardMask{};
    std::uint32_t consumedBits{};
    bool hasPayloadFingerprint{};
    bool prepared{};
};

/** Message-22 envelope identity retained until its State transaction commits and publishes. */
struct ClientStateIngress final {
    state::activity::SessionBinding binding{};
    std::uint64_t sourceGeneration{};
    std::uint64_t clientMessageSequence{};
    std::uint32_t payloadBytes{};
    bool pending{};
};

/** Decoded entity-slot demand retained only until the enclosing transaction commits. */
struct EntitySlotsRequestedIngress final {
    state::activity::SessionBinding binding{};
    std::uint64_t sourceGeneration{};
    std::uint64_t clientMessageSequence{};
    std::int32_t requestedCount{};
    bool pending{};
};

/** Exact msg-31 or msg-32 answer retained until the authenticated frame commits. */
struct AuthorityQueryIngress final {
    middleware::bap::activity_message::entity_authority::QueryAnswer answer{};
    std::uint64_t sourceGeneration{};
    bool pending{};
};

/** Exact msg-29 acknowledgement retained until the authenticated frame commits. */
struct AuthorityResetIngress final {
    middleware::bap::activity_message::entity_authority::QueryAnswer answer{};
    std::uint64_t sourceGeneration{};
    bool pending{};
};

/** The client relinquishes one bubble's authority on message 33. */
struct AuthorityAbdicationIngress final {
    middleware::bap::activity_message::entity_slots::EntitySlotMask entities{};
    std::uint64_t sourceGeneration{};
    std::uint8_t bubble{};
    bool pending{};
};

/** A purge request retains its exact mask and the next shared replication epoch. */
struct AuthorityPurgeIngress final {
    middleware::bap::activity_message::host_control::PurgeAuthorityBody body{};
    state::activity::MemberPurge departure{};
    state::activity::SessionBinding binding{};
    std::uint64_t expectedSequence{};
    std::uint64_t sourceGeneration{};
    bool pending{};
};

/** Scalar and mask data kept after the sensitive svc8 payload view expires. */
struct ActivityPlan final {
    bool hasRelayConnectivityFailure{};
    middleware::bap::activity_message::TransportReport transportReport{};
    std::uint64_t transportBindingGeneration{};
    std::uint32_t correlation{};
    std::uint64_t sessionId{};
    state::activity::entity_slots::PendingMutation entitySlotMutation{};
    state::activity::bubble_authority::EntitySlotMask returnedEntitySlots{};
    bool hasReturnedEntitySlots{};
    state::activity::membership::PendingMutation membershipMutation{};
    state::activity::reservations::PendingMutation reservationMutation{};
    JoinIngressDiagnostic joinIngress{};
    ClientStateIngress clientState{};
    EntitySlotsRequestedIngress entitySlotsRequested{};
    AuthorityQueryIngress authorityQuery{};
    AuthorityResetIngress authorityReset{};
    AuthorityAbdicationIngress authorityAbdication{};
    AuthorityPurgeIngress authorityPurge{};
    middleware::bap::activity_message::patch_epoch::PatchEpoch patchEpoch{};
    /** Exact target generation whose destination the staged msg1 must encode. */
    state::activity::SessionBinding targetBinding{};
    /** Exact advertised host row used only by a new public-target join. */
    server::gameplay::group::HostSessionBinding publicHost{};
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
    BindingIntent bindingIntent{};
};

} // namespace sunrise::server::bap::encrypted::activity_message
