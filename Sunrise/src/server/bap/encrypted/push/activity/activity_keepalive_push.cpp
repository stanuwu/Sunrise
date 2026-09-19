#include "activity_keepalive_push.h"

#include <Windows.h>

#include <array>
#include <cstdio>

#include "../../../../../core/logging/log.h"
#include "../../../../../core/settings/settings.h"
#include "../../../../../middleware/bap/activity_message/activity_replication_epoch_encoder.h"
#include "../../../../../middleware/secure_channel/runtime.h"
#include "../../../../../state/activity/definition.h"
#include "../../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../../state/activity/runtime.h"
#include "../../../../../state/runtime/runtime.h"
#include "../../../../activity/host_runtime.h"
#include "../../../../gameplay/gameplay_advertisement.h"
#include "../../../../gameplay/squad_entity_retirement.h"
#include "../../../activity_authority_query_owner.h"
#include "../../../activity_authority_reset_owner.h"
#include "../../activity_message/definition.h"
#include "../../bap_connection_publication.h"
#include "activity_arrival.h"
#include "activity_authority_query_push.h"
#include "activity_authority_reset_push.h"
#include "activity_global_state_push.h"
#include "activity_incident_push.h"
#include "activity_membership_push.h"
#include "activity_notification_frame.h"
#include "activity_roster_push.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {

namespace replication_epoch = middleware::bap::activity_message::replication_epoch;

/**
 * Drives the launch cinematic hold across one activity load, latched by `cinematicHeld`.
 * Phase A arms a family-0 banner push while the client loads, so the cinematic gate refuses and
 * skips the spaceflight legs. Phase B arms the family-4 completion once the world has arrived.
 * @param session Auth, nonce and queuez state owned by the connection.
 * @param now Tick count for the arm's due time.
 */
void drive_cinematic_hold(Session& session, std::uint64_t now) noexcept {
    if (!core::settings::get().server.gameplay.holdLaunchCinematic) {
        return;
    }
    const std::uint64_t root = session.queuez.family4RootSoid;
    if (root == 0 || !session.queuez.family0Active) {
        return;
    }
    const bool arrived = client_in_world(session, nullptr);
    if (!arrived && !session.cinematicHeld) {
        session.bannerRepushArmed = true;
        session.bannerRepushRoot = root;
        session.bannerRepushDueTick = now;
        session.cinematicHeld = true;
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         "ev=queuez stage=cinematic_hold result=park");
    } else if (arrived && session.cinematicHeld) {
        session.family4RepushArmed = true;
        session.family4RepushRoot = root;
        session.family4RepushDueTick = now;
        session.cinematicHeld = false;
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         "ev=queuez stage=cinematic_hold result=release");
    }
}

/** Failed incident attempts wait before rebuilding the same frame. */
constexpr std::uint64_t kIncidentRetryIntervalMs = 250;
/**
 * Retry cadence for a membership body held while its advertisement is still being allocated.
 * The allocation lands in the next service slice, so this is short. Leaving the keepalive due
 * instead would rebuild and send a whole frame on every pump.
 */
constexpr std::uint64_t kMembershipRetryIntervalMs = 250;
/** -1 asks for the membership snapshot without naming a bubble. */
constexpr std::int32_t kNoBubble = -1;
/** A refresh re-send carries the current revision instead of asking for an older one. */
constexpr std::uint32_t kCurrentRevision = 0;

/** Clears a staged replication epoch while keeping the request pending. */
void discard_staged_replication_epoch(Session& session) noexcept {
    session.activityReplicationEpoch.staged = false;
}

/** Commits one replication epoch only after the complete frame is published. */
void commit_staged_replication_epoch(Session& session) noexcept {
    ReplicationEpochPublication& request = session.activityReplicationEpoch;
    if (request.staged && request.bindingGeneration == session.activity.bindingGeneration) {
        session.activity.replicationEpoch = request.generation;
        request.pending = false;
    }
    request.staged = false;
}

/** Appends the exact pending activity message 44 body. */
[[nodiscard]] bool append_replication_epoch(Session& session,
                                            Scratch& scratch,
                                            std::span<const std::byte, state::kAesKeySize> key,
                                            std::array<std::byte, state::kBapNonceSize>& nonce,
                                            std::span<std::byte> response,
                                            std::size_t& written) noexcept {
    ReplicationEpochPublication& request = session.activityReplicationEpoch;
    request.staged = false;
    if (!request.pending || request.bindingGeneration != session.activity.bindingGeneration) {
        return false;
    }
    std::array<std::byte, replication_epoch::kEncodedSize> body{};
    std::size_t bodySize = 0;
    if (!replication_epoch::encode(request.generation, body, bodySize)
        || !append_notification_frame(scratch,
                                      session.activity.session.sessionId,
                                      replication_epoch::kMessageType,
                                      std::span(body).first(bodySize),
                                      key,
                                      nonce,
                                      response,
                                      written)) {
        return false;
    }
    middleware::secure_channel::advance_nonce(nonce);
    request.staged = true;
    return true;
}

/**
 * Copies one staged frame to the caller and publishes its nonce.
 * @param session Connection-owned send nonce.
 * @param scratch Lock-owned staging storage.
 * @param response Caller-owned complete-frame storage.
 * @param written Receives the published byte count.
 * @param framedSize Staged byte count.
 * @param nextSendNonce Nonce to publish once the copy finishes.
 * @param published True when at least one notification was staged.
 * @return True when the caller received a complete frame.
 */
[[nodiscard]] bool publish_frame(Session& session,
                                 Scratch& scratch,
                                 std::span<std::byte> response,
                                 std::size_t& written,
                                 std::size_t framedSize,
                                 const std::array<std::byte, state::kBapNonceSize>& nextSendNonce,
                                 bool published) noexcept {
    server::gameplay::entity_identities::PublicationLease entityLease;
    if (!published || framedSize == 0 || framedSize > response.size()
        || !begin_staged_roster_publication(session, entityLease)) {
        // Nothing left, so a roster staged into the discarded body is offered again next push.
        discard_staged_roster(session);
        discard_staged_advertisement(session);
        discard_staged_incident(session);
        discard_staged_authority_reset(session);
        discard_staged_authority_query(session);
        discard_staged_replication_epoch(session);
        return false;
    }
    for (std::size_t index = 0; index < framedSize; ++index) {
        response[index] = scratch.framed[index];
    }
    written = framedSize;
    entityLease.release();
    session.sendNonce = nextSendNonce;
    // Settled only here: the grant and the state byte may move only on a delivered frame.
    commit_staged_roster(session);
    commit_staged_advertisement(session);
    commit_staged_incident(session);
    commit_staged_authority_reset(session, GetTickCount64());
    commit_staged_authority_query(session, GetTickCount64());
    commit_staged_replication_epoch(session);
    return true;
}

/** Preserves a pending incident's backoff until it succeeds or disappears. */
void update_incident_retry(
    Session& session, std::uint64_t now, bool hasPending, bool due, bool transportStaged) noexcept {
    if (!hasPending || (due && transportStaged)) {
        session.activityIncidentRetryDueTick = 0;
    } else if (due) {
        session.activityIncidentRetryDueTick = now + kIncidentRetryIntervalMs;
    }
}

} // namespace

/** Writes the periodic activity-link keepalive when one is due. */
bool consume_activity_keepalive(Session& session,
                                Scratch& scratch,
                                std::span<std::byte> response,
                                std::size_t& written,
                                bool& touchesScratch) noexcept {
    const state::activity_sdk::CatalogPublicationGuard catalogPublication;
    written = 0;
    const std::uint64_t now = GetTickCount64();
    const bool keepaliveDue = now >= session.activityKeepaliveDueTick;
    // A region change cannot wait for the keepalive, because the client claims the next region at
    // once. Only the reported field is read here, since this runs on every pump. The client
    // routes an activity body only after its join, so nothing runs until that join committed.
    const bool active = session.activity.role != ActivityClientRole::none
                        && session.activityJoinGeneration == session.activity.bindingGeneration
                        && state::activity::binding_matches(session.activity.session)
                        && state::activity::binding_matches(session.activity.source);
    if (active) {
        static_cast<void>(authority_reset::expire(
            session.activityAuthorityReset, session.activity.bindingGeneration, now));
        static_cast<void>(authority_query::expire(
            session.activityAuthorityQuery, session.activity.bindingGeneration, now));
        drive_cinematic_hold(session, now);
    }
    const bool authorityResetDue = active
                                   && authority_reset::pending(session.activityAuthorityReset,
                                                               session.activity.bindingGeneration);
    const bool authorityQueryDue = active
                                   && authority_query::pending(session.activityAuthorityQuery,
                                                               session.activity.bindingGeneration);
    server::activity::host::AuthState hostState{};
    const bool hostStateDue =
        active && session.activityPatchEpoch.seen
        && session.activityPatchEpoch.bindingGeneration == session.activity.bindingGeneration
        && server::activity::host::auth_state(session.activity.session, hostState)
        && hostState.revision != 0 && hostState.revision != session.activityHostStateRevision;
    server::activity::host::PendingScriptableOverride pendingScriptable{};
    const bool scriptableDue =
        active && session.activityPatchEpoch.seen
        && session.activityPatchEpoch.bindingGeneration == session.activity.bindingGeneration
        && activity_link_count_locked(session.activity.session) == 1
        && server::activity::host::pending_scriptable_override(session.activity.session,
                                                               pendingScriptable);
    server::activity::host::PendingIncident pendingIncident{};
    const bool hasPendingIncident =
        active
        && server::activity::host::pending_incident(session.activity.session, pendingIncident);
    const bool incidentClientReady = client_in_world(session, nullptr);
    const bool incidentDue =
        hasPendingIncident && incidentClientReady && now >= session.activityIncidentRetryDueTick;
    const bool isPrivate = session.activity.role == ActivityClientRole::privateCurrent;
    // The arrival report (ws 702 world state 8) is answered by one roster built after it, on each
    // link whose delivered body still holds the wait bit. That answer raises `entered`.
    const bool arrivalDue = active && session.activityRosterAwaitClientSync && incidentClientReady;
    // A script-selected state revision that has not reached the client's roster. Nothing else
    // makes a roster due for it, so it would otherwise wait for the next solicited report.
    const MissionSeedLease& missionSeed = session.activityMissionSeed;
    const bool missionSeedDue =
        active && missionSeed.configured
        && missionSeed.bindingGeneration == session.activity.bindingGeneration
        && missionSeed.revision != missionSeed.publishedRevision
        && !missionSeed.regionArrivalPending && client_region_ready(session, nullptr);
    // The region the client holds, which is the one its advertisement must describe. The pending
    // leg alone names the region behind the player after a z-leg switch, and advertising that
    // hands the client an ambassadorship for a region it has left.
    const std::int32_t reportedRegion =
        active && isPrivate
            ? state::activity::membership::player_region(session.activity.source.sessionId)
            : -1;
    // A private region advertises its own Bubble Host, so a move into one owes a send too. A move
    // the other way keeps the index when the client returns to where it came from, and only the
    // kind of advertisement changes, so the kind is compared as well.
    const bool reportedPrivate = reportedRegion >= 0 && private_region(session, reportedRegion);
    // Until the client reports a region the advertisement follows the arrival, or the first record
    // it adopts for that region names it ambassador. The private Bubble Host row stays where the
    // join claimed it; the delivered body retains it.
    bool arrivalChanged = false;
    std::int32_t arrivalRegion = -1;
    if (active && isPrivate && reportedRegion < 0) {
        const EffectiveRegion arrival = effective_region(session.activity.session);
        server::gameplay::group::HostSessionBinding privateHost{};
        const std::int32_t advertised =
            session.activity.advertisedRegion >= 0 ? session.activity.advertisedRegion
            : server::gameplay::private_host_session(session.activity.session, privateHost)
                ? privateHost.regionIndex
                : -1;
        if (!arrival.reported && arrival.index >= 0 && arrival.index != advertised) {
            arrivalChanged = true;
            arrivalRegion = arrival.index;
            if (!private_region(session, arrival.index)) {
                server::gameplay::complete_host_session(session.activity.session, arrival.index);
            }
            std::array<char, core::log::kLineCapacity> line{};
            const int length = std::snprintf(line.data(),
                                             line.size(),
                                             "ev=gameplay stage=arrival result=changed region=%d "
                                             "previous=%d",
                                             arrival.index,
                                             advertised);
            if (length > 0) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(length)});
            }
        }
    }
    const bool regionChanged = isPrivate
                               && ((reportedRegion >= 0
                                    && (reportedRegion != session.activity.advertisedRegion
                                        || reportedPrivate != session.activity.advertisedPrivate))
                                   || arrivalChanged);
    if (active && isPrivate) {
        std::array<char, core::log::kLineCapacity> line{};
        const int length = std::snprintf(
            line.data(),
            line.size(),
            "ev=activity stage=advertise_trigger held=%d pending=%d advertised=%d "
            "held_private=%u advertised_private=%u changed=%u",
            reportedRegion,
            state::activity::membership::reported_region(session.activity.source.sessionId),
            session.activity.advertisedRegion,
            reportedPrivate ? 1U : 0U,
            session.activity.advertisedPrivate ? 1U : 0U,
            regionChanged ? 1U : 0U);
        if (length > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::debug,
                             {line.data(), static_cast<std::size_t>(length)});
        }
    }
    // Arming a host teleport or a hard wipe is a host-owned event and cannot wait for the keepalive
    // cadence. It needs an acknowledged revision, so it fires once per ack and stops until the
    // client acks or reports.
    const bool hostTeleportDue =
        active
        && (state::activity::membership::host_teleport_armed(session.activity.session.sessionId)
            || state::activity::membership::hard_wipe_start_unseen(
                session.activity.session.sessionId))
        && state::activity::membership::acknowledged(session.activity.session.sessionId);
    const bool retirementReady =
        active && session.activityPatchEpoch.seen
        && session.activityPatchEpoch.bindingGeneration == session.activity.bindingGeneration
        && activity_link_count_locked(session.activity.session) == 1;
    bool retirementDue = retirementReady
                         && server::gameplay::squad_entity_retirement::placed_transition_pending(
                             session.activity.session, session.activity.bindingGeneration);
    if (retirementReady && !retirementDue) {
        const auto region = effective_region(session.activity.session);
        const auto bubble =
            region.index >= 0
                ? region.index >> state::activity::bubble_authority::kSliceSetToBubbleShift
                : -1;
        if (bubble >= 0 && bubble != session.activityRosterRegionBubble) {
            server::gameplay::squad_entity_retirement::RetirementPlan retirement{};
            retirementDue = server::gameplay::squad_entity_retirement::prepare_retirement(
                                session.activity.session,
                                session.activity.bindingGeneration,
                                static_cast<std::uint8_t>(bubble),
                                retirement)
                            && retirement.pending;
        }
    }
    if (!active
        || (!keepaliveDue && !regionChanged && !hostStateDue && !scriptableDue && !incidentDue
            && !authorityResetDue && !authorityQueryDue && !hostTeleportDue && !retirementDue
            && !arrivalDue && !missionSeedDue)) {
        return false;
    }
    touchesScratch = true;

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    const auto& key = session.sessionKey;
    bool published = false;
    // Held until the frame reaches the caller. Encoding alone does not spend the region trigger.
    std::int32_t stagedAdvertisedRegion = -1;
    bool stagedAdvertisedPrivate = false;
    // Only a changed Host value, retirement or the arrival answer can publish a standalone roster.
    if (!keepaliveDue && !regionChanged && !hostTeleportDue) {
        bool appendedRoster = false;
        if (hostStateDue || scriptableDue || retirementDue || arrivalDue || missionSeedDue) {
            appendedRoster = append_roster_notification(
                session, scratch, key, nextSendNonce, scratch.framed, framedSize);
            published = appendedRoster;
        }
        const bool appendedIncident =
            incidentDue
            && append_incident_notification(
                session, scratch, pendingIncident, key, nextSendNonce, scratch.framed, framedSize);
        published = appendedIncident || published;
        const bool appendedAuthorityReset =
            authorityResetDue
            && append_authority_reset_notification(
                session, scratch, key, nextSendNonce, scratch.framed, framedSize);
        published = appendedAuthorityReset || published;
        const bool appendedAuthorityQuery =
            authorityQueryDue
            && append_authority_query_notification(
                session, scratch, key, nextSendNonce, scratch.framed, framedSize);
        published = appendedAuthorityQuery || published;
        const bool delivered = publish_frame(
            session, scratch, response, written, framedSize, nextSendNonce, published);
        if (delivered && (appendedIncident || appendedAuthorityReset || appendedAuthorityQuery)) {
            session.activityKeepaliveDueTick = now + kActivityKeepaliveIntervalMs;
        }
        update_incident_retry(
            session, now, hasPendingIncident, incidentDue, delivered && appendedIncident);
        return delivered;
    }
    published = append_global_state_notification(
        scratch, session.activity.session, key, nextSendNonce, scratch.framed, framedSize);
    const bool appendedReplicationEpoch =
        append_replication_epoch(session, scratch, key, nextSendNonce, scratch.framed, framedSize);
    published = appendedReplicationEpoch || published;
    // Nothing may advance membership State until the first required frame proves this caller owns
    // enough capacity to publish at least the keepalive prefix.
    if (!published || framedSize > response.size()) {
        static_cast<void>(publish_frame(
            session, scratch, response, written, framedSize, nextSendNonce, published));
        update_incident_retry(session, now, hasPendingIncident, incidentDue, false);
        return false;
    }
    const bool appendedAuthorityReset =
        authorityResetDue
        && append_authority_reset_notification(
            session, scratch, key, nextSendNonce, scratch.framed, framedSize);
    published = appendedAuthorityReset || published;
    const bool appendedAuthorityQuery =
        authorityQueryDue
        && append_authority_query_notification(
            session, scratch, key, nextSendNonce, scratch.framed, framedSize);
    published = appendedAuthorityQuery || published;
    if (session.activity.role == ActivityClientRole::publicTarget) {
        // The target owns its epoch and roster but advertises no target. Msg 12 must bind its world
        // container before a grant reaches it. Send once per binding: it has no acknowledgement or
        // region report to close another gate.
        state::activity::membership::PendingMutation staged{};
        bool appended = false;
        const bool owesMembership =
            core::settings::get().server.activation.activityPublicMembership
            && session.activityMembershipSentGeneration != session.activity.bindingGeneration;
        if (owesMembership) {
            // Copy the private source table: only it carries local machine and character identity.
            // This target cannot advance that table, so capture it without committing and use the
            // exact source binding rather than whichever private session is newest.
            const std::uint64_t privateSessionId = session.activity.source.sessionId;
            // Non-zero from the private link's own seed, which lands before the client sends its
            // identity message. It proves only that there is a member table to copy.
            const bool identityPublished =
                privateSessionId != state::activity::kAbsentSessionId
                && state::activity::membership::join_identity(privateSessionId) != 0;
            const bool hasSnapshot = identityPublished
                                     && state::activity::membership::prepare_refresh(
                                         privateSessionId, kCurrentRevision, kNoBubble, staged)
                                     && staged.hasSnapshot;
            if (hasSnapshot) {
                activity_message::ActivityPlan plan{};
                plan.sessionId = session.activity.session.sessionId;
                plan.membershipMutation = staged;
                appended = append_membership_notification(
                    scratch, session, plan, key, nextSendNonce, scratch.framed, framedSize);
                published = appended || published;
                SecureZeroMemory(&plan, sizeof plan);
            }
        }
        if (appended || hostStateDue || scriptableDue || retirementDue || arrivalDue
            || missionSeedDue) {
            published = append_roster_notification(
                            session, scratch, key, nextSendNonce, scratch.framed, framedSize)
                        || published;
        }
        const bool appendedIncident =
            incidentDue
            && append_incident_notification(
                session, scratch, pendingIncident, key, nextSendNonce, scratch.framed, framedSize);
        published = appendedIncident || published;
        SecureZeroMemory(&staged, sizeof staged);
        const bool delivered = publish_frame(
            session, scratch, response, written, framedSize, nextSendNonce, published);
        if (delivered) {
            // Latched here, not at encode. An encoded body the client never saw is not a send.
            if (appended) {
                note_activity_membership_delivery(session);
                commit_membership_body_record(session);
                // The join burst owns this body. Reaching here means it had no snapshot to send,
                // and this copy lands mid-transition instead.
                core::log::write(
                    core::log::Channel::server,
                    core::log::Level::warn,
                    "ev=activity stage=membership result=late reason=join_burst_empty");
            }
            session.activityKeepaliveDueTick = now + kActivityKeepaliveIntervalMs;
        }
        update_incident_retry(
            session, now, hasPendingIncident, incidentDue, delivered && appendedIncident);
        return delivered;
    }

    // Republish only when a real advertisement changes the acknowledged membership revision.
    // Membership becomes publishable after identity arrives, so it rides the keepalive.
    state::activity::membership::PendingMutation refresh{};
    state::activity::membership::PendingMutation stagedMembership{};
    const bool advertisedRegionReady = regionChanged
                                       && region_advertisement(session, reportedRegion)
                                              == server::gameplay::AdvertisementState::ready;
    // An armed host teleport also needs a revision advance. The client applies one update per
    // revision, so a refresh at the current revision would carry the block and be ignored. The
    // arm stops counting as armed once the client reports the region, which ends this.
    const bool needsRepublish =
        (advertisedRegionReady
         || state::activity::membership::host_teleport_armed(session.activity.session.sessionId)
         || state::activity::membership::hard_wipe_needs_publish(
             session.activity.session.sessionId))
        && state::activity::membership::acknowledged(session.activity.session.sessionId);
    bool preparedRepublish = needsRepublish
                             && state::activity::membership::prepare_republish(
                                 session.activity.session.sessionId, stagedMembership);
    bool commitsMembership = preparedRepublish;
    bool hasMembership = false;
    if (preparedRepublish) {
        refresh = stagedMembership;
        hasMembership = true;
    } else {
        hasMembership =
            state::activity::membership::prepare_refresh(
                session.activity.session.sessionId, kCurrentRevision, kNoBubble, refresh)
            && refresh.hasSnapshot;
    }
    if (!hasMembership
        && prepare_seed_identity(session.activity.session.sessionId,
                                 session.activityMemberKey,
                                 session.activityCharacterSoid,
                                 stagedMembership)) {
        refresh = stagedMembership;
        hasMembership = stagedMembership.hasSnapshot;
        commitsMembership = true;
    }
    // The citizen advertisement rides on this message. Without one more send per region the client
    // finds no ambassador in the next region, takes the role itself and matchmakes forever.
    // Re-sending a stable snapshot instead would make it rebuild every player snapshot.
    const bool owesIdentityReflection =
        session.activityClientIdentitySeenGeneration == session.activity.bindingGeneration
        && session.activityClientIdentityPublishedGeneration != session.activity.bindingGeneration;
    // A prepared republish must reach the wire even when nothing else changed. It carries the
    // armed host teleport at a new revision. Without this it is staged and then dropped, so the
    // client never receives the move and the transition never starts.
    const bool publishesMembership =
        hasMembership
        && (commitsMembership || owesIdentityReflection || regionChanged
            || !state::activity::membership::acknowledged(session.activity.session.sessionId));
    // Resolved the way the body resolves it: the pending leg the client reported, else the
    // current one, else the arrival slice set. A hold decided on any other region lets the body
    // out before its host row exists, and the region trigger then latches on that send.
    const EffectiveRegion plannedRegion =
        session.activity.role == ActivityClientRole::privateCurrent
            ? private_planned_region(refresh, session.activity.source)
            : planned_region(refresh, session.activity.source);
    const server::gameplay::AdvertisementState advertisement =
        publishesMembership ? region_advertisement(session, plannedRegion.index)
                            : server::gameplay::AdvertisementState::absent;
    if (active && isPrivate && publishesMembership) {
        std::array<char, core::log::kLineCapacity> bodyLine{};
        const int bodyLength =
            std::snprintf(bodyLine.data(),
                          bodyLine.size(),
                          "ev=activity stage=advertise_body planned=%d planned_reported=%u "
                          "trigger_held=%d adv_state=%u changed=%u",
                          plannedRegion.index,
                          plannedRegion.reported ? 1U : 0U,
                          reportedRegion,
                          static_cast<unsigned>(advertisement),
                          regionChanged ? 1U : 0U);
        if (bodyLength > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::debug,
                             {bodyLine.data(), static_cast<std::size_t>(bodyLength)});
        }
    }
    bool appendedMembership = false;
    if (publishesMembership && advertisement == server::gameplay::AdvertisementState::pending) {
        // The client applies one membership update per revision. A push made while the
        // advertisement is still being allocated spends that revision on a region record with no
        // join descriptor. The allocation lands in the next slice, so holding costs one poll.
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         "ev=gameplay stage=membership result=held reason=no_host_session");
    } else if (publishesMembership) {
        activity_message::ActivityPlan plan{};
        plan.sessionId = session.activity.session.sessionId;
        plan.membershipMutation = refresh;
        // Only a plain periodic re-send may be skipped as unchanged. A body that commits State,
        // reflects the client identity, or answers a region move must reach the wire.
        const bool suppressUnchanged =
            !commitsMembership && !owesIdentityReflection && !regionChanged;
        const bool sent = append_membership_notification(scratch,
                                                         session,
                                                         plan,
                                                         key,
                                                         nextSendNonce,
                                                         scratch.framed,
                                                         framedSize,
                                                         suppressUnchanged);
        appendedMembership = sent;
        // Only `pending` leaves the trigger armed, because only `pending` is transient. `absent`
        // means this channel advertises nothing, so re-arming there republishes on every poll.
        if (sent && reportedRegion >= 0) {
            stagedAdvertisedRegion = reportedRegion;
            stagedAdvertisedPrivate = reportedPrivate;
        } else if (sent && arrivalChanged) {
            stagedAdvertisedRegion = arrivalRegion;
            stagedAdvertisedPrivate = private_region(session, arrivalRegion);
        }
        published = sent || published;
        SecureZeroMemory(&plan, sizeof plan);
    }
    // The mirrored host state is captured before the secure clear, because the report below shows
    // whether the client asked for a state this host must not publish.
    const int reportedSpawnState = static_cast<int>(refresh.snapshot.spawn.state);
    const int reportedTeleportState = static_cast<int>(refresh.snapshot.teleport.state);
    const std::int32_t reportedTeleportSlice = refresh.snapshot.teleport.sliceSetIndex;
    const std::uint8_t reportedToken = refresh.snapshot.transitionToken;
    // The client applies one membership update per revision, so the revision is what says whether a
    // push could have been read at all. Without it a correct body and a deduped one look the same.
    const std::uint32_t reportedRevision = refresh.snapshot.revision;
    SecureZeroMemory(&refresh, sizeof refresh);
    if (appendedMembership || hostStateDue || scriptableDue || retirementDue || arrivalDue
        || missionSeedDue) {
        published = append_roster_notification(
                        session, scratch, key, nextSendNonce, scratch.framed, framedSize)
                    || published;
    }
    const bool appendedIncident =
        incidentDue
        && append_incident_notification(
            session, scratch, pendingIncident, key, nextSendNonce, scratch.framed, framedSize);
    published = appendedIncident || published;

    std::array<char, core::log::kLineCapacity> line{};
    const int count =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=activity stage=keepalive result=%s bytes=%zu membership=%u key=0x%llX "
                      "spawn_state=%d teleport_state=%d teleport_slice=%d token=%u revision=%u "
                      "advert=%u",
                      published ? "ok" : "fail",
                      framedSize,
                      hasMembership ? 1U : 0U,
                      static_cast<unsigned long long>(session.activityMemberKey),
                      reportedSpawnState,
                      reportedTeleportState,
                      reportedTeleportSlice,
                      static_cast<unsigned>(reportedToken),
                      reportedRevision,
                      static_cast<unsigned>(advertisement));
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(count)});
    }
    const bool deferredMembership = commitsMembership && !appendedMembership;
    if (framedSize > response.size()
        || (commitsMembership && appendedMembership
            && !state::activity::membership::commit(stagedMembership))) {
        SecureZeroMemory(&stagedMembership, sizeof stagedMembership);
        discard_staged_roster(session);
        discard_staged_advertisement(session);
        discard_staged_incident(session);
        discard_staged_authority_reset(session);
        discard_staged_authority_query(session);
        update_incident_retry(session, now, hasPendingIncident, incidentDue, false);
        return false;
    }
    SecureZeroMemory(&stagedMembership, sizeof stagedMembership);
    const bool delivered =
        publish_frame(session, scratch, response, written, framedSize, nextSendNonce, published);
    if (delivered && appendedMembership) {
        note_activity_membership_delivery(session);
        commit_membership_body_record(session);
    }
    // A body the client never saw must advertise its region again on the next poll.
    if (delivered && stagedAdvertisedRegion >= 0) {
        session.activity.advertisedRegion = stagedAdvertisedRegion;
        session.activity.advertisedPrivate = stagedAdvertisedPrivate;
    }
    if (delivered) {
        // A held membership body is owed again soon, but not on every pump.
        session.activityKeepaliveDueTick =
            now + (deferredMembership ? kMembershipRetryIntervalMs : kActivityKeepaliveIntervalMs);
        if (preparedRepublish && appendedMembership) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             regionChanged
                                 ? "ev=gameplay stage=membership result=republished reason=region"
                                 : "ev=gameplay stage=membership result=republished "
                                   "reason=host_teleport");
        }
    }
    update_incident_retry(
        session, now, hasPendingIncident, incidentDue, delivered && appendedIncident);
    return delivered;
}

} // namespace sunrise::server::bap::encrypted::push::activity
