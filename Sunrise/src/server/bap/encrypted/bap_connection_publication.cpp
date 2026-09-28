#include "bap_connection_publication.h"

#include <Windows.h>

#include <atomic>
#include <limits>

#include "../../../core/logging/log.h"
#include "../../../state/activity/member_context.h"
#include "../../../state/activity/member_departure.h"
#include "../../../state/activity/runtime.h"
#include "../../gameplay/group/group_host_sessions.h"
#include "../activity_transport_publication.h"
#include "push/activity/internal.h"

namespace sunrise::server::bap::encrypted {
namespace {

// The client writes its queuez record state just after sending the subscribe, so the first copy is
// refused. TODO: send on the client's record-state report; svc-12 decodes only subscribe.
/** Delay before the second Family-4 snapshot. */
constexpr std::uint64_t kFamily4RepushDelayMs = 400;
/** The banner pair lands the same unsolicited way and hits the same record-state race. */
constexpr std::uint64_t kBannerRepushDelayMs = 400;
/**
 * Delay before the ability-icon re-derivation owed by a subclass selection.
 * The Client content-extraction pump that rebuilds the invalidated ability buckets runs on the
 * next few RunCallbacks pumps, well under this window.
 */
constexpr std::uint64_t kAbilityRefreshDelayMs = 500;

/** Process-lifetime generation that rejects delayed epochs after a BAP slot is reused. */
std::atomic<std::uint64_t> g_nextActivityBindingGeneration{1};

/** Releases one host-session retain when it is present. */
void release_host_generation(std::uint64_t generation) noexcept {
    if (generation != 0) {
        server::gameplay::group::release_host_session(generation);
    }
}

/** Releases every retained host row of one directory and clears it. */
void release_host_generations(AdvertisementRetains& retains) noexcept {
    for (std::size_t entry = 0; entry < retains.count; ++entry) {
        release_host_generation(retains.hostGenerations[entry]);
    }
    retains = {};
}

/** Releases the old binding while a copied join reply may already own its replacement body. */
void release_activity_ownership(Session& session, bool preserveJoinPublication) noexcept;

/** Clears all connection state rebuilt by a successful activity join. */
void reset_join_state(Session& session) noexcept {
    clear_activity_transport(session);
    session.activityMemberKey = 0;
    session.activityJoinGeneration = 0;
    session.activityJoinCorrelation = 0;
    session.activityMemberSet = {};
    session.activityCharacterSoid = 0;
    session.activityKeepaliveDueTick = 0;
    session.activityClientIdentitySeenGeneration = 0;
    session.activityClientIdentityPublishedGeneration = 0;
    session.activityPatchEpoch = {};
    session.activityReplicationEpoch = {};
    session.activityRosterGroupLeases = {};
    session.activityRosterBubbleOrderCount = 0;
    session.activityRosterBubbleKeyOrderCount = {};
    session.activityRosterSends = 0;
    session.activityRosterRegionBubble = -1;
    session.activityHostStateRevision = 0;
    session.activityRosterAwaitClientSync = false;
    authority_query::reset(session.activityAuthorityQuery, session.activity.bindingGeneration);
    authority_reset::reset(session.activityAuthorityReset, session.activity.bindingGeneration);
    session.activityIncidentStaged = {};
    session.activityIncidentRetryDueTick = 0;
    session.activityRosterReason = 0;
    session.activityRosterStaged = {};
    session.activityRosterDecode = {};
    SecureZeroMemory(&session.activitySquadOverride, sizeof session.activitySquadOverride);
    session.activityMissionSeed = {};
    if (session.activity.role == ActivityClientRole::privateCurrent) {
        session.activity.advertisedRegion = -1;
    }
}

} // namespace

/** Reserves one process-lifetime ActivityClient generation without wrapping. */
bool reserve_activity_binding_generation(std::uint64_t& generation) noexcept {
    generation = 0;
    std::uint64_t current = g_nextActivityBindingGeneration.load();
    while (current != (std::numeric_limits<std::uint64_t>::max)()) {
        if (g_nextActivityBindingGeneration.compare_exchange_weak(current, current + 1)) {
            generation = current;
            return true;
        }
    }
    return false;
}

/** Captures the connection fields one service outcome carries. */
ConnectionFields connection_fields(const ServiceOutcome& outcome) noexcept {
    ConnectionFields fields{};
    fields.answersActivityStartup =
        transaction_if<state::activity::PendingAllocation>(outcome) != nullptr;
    fields.startupReservations = outcome.startupReservations;
    const auto* plan = transaction_if<activity_message::ActivityPlan>(outcome);
    if (plan == nullptr) {
        return fields;
    }
    if (plan->mutationDomain == activity_message::MutationDomain::membership
        && plan->membershipMutation.kind
               == state::activity::membership::MutationKind::authoritative) {
        fields.transportReport = plan->transportReport;
        fields.transportBindingGeneration = plan->transportBindingGeneration;
    }
    if (plan->delivery == activity_message::Delivery::joinNotifications) {
        fields.joinMemberKey = plan->entitySlotMutation.memberKey;
        fields.joinCorrelation = plan->correlation;
        fields.sharedJoin = plan->entitySlotMutation.shared;
        fields.joinCharacterSoid = plan->joinCharacterSoid;
        fields.joinIngress = plan->joinIngress;
        fields.joinsActivity = true;
    }
    if (plan->mutationDomain == activity_message::MutationDomain::patchEpoch) {
        fields.patchEpoch = plan->patchEpoch;
        fields.retainsPatchEpoch = true;
    }
    fields.receivesClientIdentity =
        plan->mutationDomain == activity_message::MutationDomain::membership
        && plan->membershipMutation.kind == state::activity::membership::MutationKind::identity;
    return fields;
}

/** Records one membership body after its complete frame reaches the transport caller. */
void note_activity_membership_delivery(Session& session) noexcept {
    const std::uint64_t generation = session.activity.bindingGeneration;
    if (session.activity.role == ActivityClientRole::none || generation == 0) {
        return;
    }
    session.activityMembershipSentGeneration = generation;
    if (session.activityClientIdentitySeenGeneration == generation) {
        session.activityClientIdentityPublishedGeneration = generation;
    }
}

/** Publishes the captured connection fields after a successful commit. */
void publish_connection_fields(Session& session,
                               const transactions::Publication& publication,
                               const ConnectionFields& fields) noexcept {
    if (publication.hasActivitySessionBinding) {
        if (session.activity.bindingGeneration != publication.activity.bindingGeneration) {
            server::activity::host::retire_scriptable_client(session.activity.session,
                                                             session.activity.bindingGeneration);
        }
        if (!publication.preservesActivitySessionBinding) {
            release_activity_ownership(
                session, fields.joinsActivity && session.activityJoinMembershipStaged);
            session.activity = publication.activity;
            reset_join_state(session);
        }
        session.activity.bindingGeneration = publication.activity.bindingGeneration;
    }
    if (fields.answersActivityStartup && publication.hasActivitySessionBinding) {
        session.activityStartupReservations = fields.startupReservations;
        session.activityStartupReservations.sessionId = session.activity.session.sessionId;
        session.activityStartupReservations.createdRevision =
            session.activity.session.createdRevision;
    }
    if (fields.joinsActivity) {
        // The join burst may have staged its own membership directory; only a stale one goes.
        if (!session.activityJoinMembershipStaged) {
            discard_staged_advertisement(session);
        }
        release_host_generations(session.activityAdvertisementHeld);
        reset_join_state(session);
        session.activity.replicationSequence = publication.activity.replicationSequence;
        session.activity.replicationEpoch = publication.activity.replicationEpoch;
        session.activityMemberKey = fields.joinMemberKey;
        session.activityJoinGeneration = session.activity.bindingGeneration;
        session.activityJoinCorrelation = fields.joinCorrelation;
        session.activityCharacterSoid = fields.joinCharacterSoid;
        state::activity::JoinedMemberSet members{};
        if (state::activity::joined_member_set(
                session.activity.session, session.activityMemberKey, members)) {
            session.activityMemberSet = members.keys;
        }
        // A committed native join supersedes this account's old ActivityClient on the same
        // activity generation. Publish the replacement first so old-link retirement cannot
        // withdraw its surviving member or release another region's owner.
        for (auto& other : sessions()) {
            if (&other != &session && other.id && other.authenticated
                && other.accountHandle == session.accountHandle && other.activityJoinGeneration
                && state::activity::same_binding(other.activity.session, session.activity.session)
                && (other.activityMemberKey != session.activityMemberKey
                    || other.activityJoinCorrelation != session.activityJoinCorrelation)) {
                release_activity_connection(other);
            }
        }
    }
    const state::activity::ScopedMemberContext currentMemberScope(
        session.activity.session.sessionId, session.activityMemberKey);
    if (fields.retainsPatchEpoch) {
        session.activityPatchEpoch.value = fields.patchEpoch;
        session.activityPatchEpoch.bindingGeneration = session.activity.bindingGeneration;
        session.activityPatchEpoch.seen = session.activity.role != ActivityClientRole::none;
    }
    if (fields.receivesClientIdentity) {
        session.activityClientIdentitySeenGeneration = session.activity.bindingGeneration;
    }
    publish_activity_transport(session, fields.transportBindingGeneration, fields.transportReport);
    // A join resets the roster container. Its first post-region state change rebuilds the
    // participation component against the published membership.
    if (fields.joinsActivity) {
        session.activityRosterSends = 0;
        session.activityRosterGroupLeases = {};
        session.activityRosterBubbleOrderCount = 0;
        session.activityRosterBubbleKeyOrderCount = {};
        session.activityRosterRegionBubble = -1;
    }
    // A private join burst delivered the seed membership body; commit the matching identity so
    // State and the delivered-body record agree with what the client now holds.
    if (fields.joinsActivity && session.activity.role == ActivityClientRole::privateCurrent) {
        if (session.activityJoinMembershipStaged) {
            push::activity::adopt_join_membership_record(session);
        }
        state::activity::membership::PendingMutation seed{};
        if (!fields.sharedJoin
            && (!push::activity::prepare_seed_identity(session.activity.session.sessionId,
                                                       session.activityMemberKey,
                                                       session.activityCharacterSoid,
                                                       seed)
                || !state::activity::membership::commit(seed))) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=activity stage=membership result=seed_fail");
        }
    }
    if (fields.joinsActivity) {
        static_cast<void>(activity_host_manager::admit_startup_reservations(
            session.activityStartupReservations,
            session.activity.session.sessionId,
            session.activity.session.createdRevision,
            state::account_primary_soid(session.accountHandle),
            session.activityMemberKey,
            session.activityCharacterSoid));
    }
    // Reached only once the frame has been copied out, so this is the delivery the one-shot means.
    const bool deliveredJoinMembership =
        fields.joinsActivity && session.activityJoinMembershipStaged;
    if (deliveredJoinMembership || session.activityAdvertisementStaged.staged) {
        // A public or shared target burst delivers its membership body here, so adopting it gives
        // this connection the revision cursor the keepalive reads. A second adopt is a no-op.
        if (deliveredJoinMembership) {
            push::activity::adopt_join_membership_record(session);
        } else {
            // Replies and refreshes keep their binding, but delivered the staged body too.
            push::activity::commit_membership_body_record(session);
        }
        note_activity_membership_delivery(session);
    }
    session.activityJoinMembershipStaged = false;
}

/** Stages one body's retained host directory until the membership frame has an outcome. */
void stage_activity_advertisement(Session& session, const AdvertisementRetains& retains) noexcept {
    // The new membership body is already staged. Release only the previous directory here;
    // the failure path discards both the body and its matching directory together.
    release_host_generations(session.activityAdvertisementStaged.retains);
    session.activityAdvertisementStaged.retains = retains;
    session.activityAdvertisementStaged.staged = true;
}

/** Publishes one staged directory's retains. */
void commit_staged_advertisement(Session& session) noexcept {
    if (!session.activityAdvertisementStaged.staged) {
        return;
    }
    release_host_generations(session.activityAdvertisementHeld);
    session.activityAdvertisementHeld = session.activityAdvertisementStaged.retains;
    session.activityAdvertisementStaged = {};
}

/** Releases one staged directory's retains. */
void discard_staged_advertisement(Session& session) noexcept {
    push::activity::discard_membership_body_record(session);
    if (session.activityAdvertisementStaged.staged) {
        release_host_generations(session.activityAdvertisementStaged.retains);
        session.activityAdvertisementStaged = {};
    }
}

namespace {
void release_activity_ownership(Session& session, bool preserveJoinPublication) noexcept {
    server::activity::host::retire_scriptable_client(session.activity.session,
                                                     session.activity.bindingGeneration);
    session.activityStartupReservations = {};
    if (session.activityMemberKey && session.activityJoinGeneration != 0
        && session.activityJoinGeneration == session.activity.bindingGeneration) {
        bool retainedByPeer{};
        for (const auto& other : sessions()) {
            if (&other != &session && other.id && other.authenticated
                && other.accountHandle == session.accountHandle
                && other.activityMemberKey == session.activityMemberKey
                && other.activityJoinGeneration != 0
                && other.activityJoinGeneration == other.activity.bindingGeneration
                && state::activity::same_binding(other.activity.session,
                                                 session.activity.session)) {
                retainedByPeer = true;
                break;
            }
        }
        if (!retainedByPeer) {
            if (!state::activity::depart_member(session.activity.session,
                                                state::account_primary_soid(session.accountHandle),
                                                session.activityMemberKey)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=activity stage=disconnect_departure result=refused");
            }
        }
    }
    clear_activity_transport(session);
    if (!preserveJoinPublication) {
        discard_staged_advertisement(session);
    }
    release_host_generations(session.activityAdvertisementHeld);
    if (session.activity.hostGeneration != 0) {
        server::gameplay::group::release_host_session(session.activity.hostGeneration);
    }
    if (session.activity.session.sessionId != state::activity::kAbsentSessionId) {
        state::activity::release_binding(session.activity.session);
    }
    session.activity = {};
    session.activityJoinCorrelation = 0;
    session.activityMemberSet = {};
    authority_query::reset(session.activityAuthorityQuery, 0);
    authority_reset::reset(session.activityAuthorityReset, 0);
    session.activityPatchEpoch = {};
    session.activityReplicationEpoch = {};
    session.activityJoinGeneration = 0;
    session.activityClientIdentitySeenGeneration = 0;
    session.activityClientIdentityPublishedGeneration = 0;
    session.activityRosterStaged.decodeMap = {};
    session.activityRosterDecode = {};
}

} // namespace

/** Releases every exact activity owner held by one BAP connection. */
void release_activity_connection(Session& session) noexcept {
    release_activity_ownership(session, false);
}

/** Arms the owed Family-4 and banner re-pushes when the queuez publication asks for them. */
void arm_repushes(Session& session, const queuez::StagedPublication& queuezPublication) noexcept {
    const std::uint64_t now = GetTickCount64();
    if (queuezPublication.armsAbilityRefresh) {
        session.abilityRefreshDueTick = now + kAbilityRefreshDelayMs;
        if (session.characterRefreshScope == CharacterRefreshScope::none) {
            session.characterRefreshScope = CharacterRefreshScope::records;
        }
    }
    if (queuezPublication.armsFamily4Repush && queuezPublication.family4RepushRoot != 0) {
        session.family4RepushDueTick = now + kFamily4RepushDelayMs;
        session.family4RepushRoot = queuezPublication.family4RepushRoot;
        session.family4RepushArmed = true;
    }
    // Armed on its own signal, not on family four's. Family zero re-subscribes on every record
    // cycle, and each subscribe needs a delayed copy because the immediate answer arrives too soon.
    if (queuezPublication.armsBannerRepush && queuezPublication.bannerRepushRoot != 0) {
        session.bannerRepushDueTick = now + kBannerRepushDelayMs;
        session.bannerRepushRoot = queuezPublication.bannerRepushRoot;
        session.bannerRepushArmed = true;
    }
    // Root zero means the peer never subscribed to family two, so it is owed no re-push.
    if (queuezPublication.socialRosterRepushRoot != 0) {
        session.socialRosterRepushRoot = queuezPublication.socialRosterRepushRoot;
    }
    // Armed by the equip that moved the member record. Re-arming coalesces, so a burst owes one
    // send.
    if (queuezPublication.rearmsSocialRosterRepush && session.socialRosterRepushRoot != 0) {
        session.socialRosterRepushArmed = true;
    }
}

} // namespace sunrise::server::bap::encrypted
