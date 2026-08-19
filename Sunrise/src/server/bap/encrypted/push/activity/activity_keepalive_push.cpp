#include "activity_keepalive_push.h"

#include <Windows.h>

#include <array>
#include <cstdio>

#include "../../../../../core/logging/log.h"
#include "../../../../../core/settings/settings.h"
#include "../../../../../state/activity/definition.h"
#include "../../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../../state/runtime/runtime.h"
#include "../../../../gameplay/gameplay_advertisement.h"
#include "../../activity_message/definition.h"
#include "../../bap_connection_publication.h"
#include "activity_arrival.h"
#include "activity_global_state_push.h"
#include "activity_membership_push.h"
#include "activity_roster_push.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {

/**
 * Roster burst cadence, used only while the client is loading. Outside that window the roster
 * rides the keepalive.
 */
constexpr std::uint64_t kRosterBurstIntervalMs = 1'000;
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
    if (!published || framedSize == 0 || framedSize > response.size()) {
        // Nothing left, so a roster staged into the discarded body is offered again next push.
        discard_staged_roster(session);
        discard_staged_advertisement(session);
        return false;
    }
    for (std::size_t index = 0; index < framedSize; ++index) {
        response[index] = scratch.framed[index];
    }
    written = framedSize;
    session.sendNonce = nextSendNonce;
    // Settled only here: the grant and the state byte may move only on a delivered frame.
    commit_staged_roster(session);
    commit_staged_advertisement(session);
    return true;
}

} // namespace

/** Writes the periodic activity-link keepalive when one is due. */
bool consume_activity_keepalive(Session& session,
                                Scratch& scratch,
                                std::span<std::byte> response,
                                std::size_t& written,
                                bool& touchesScratch) noexcept {
    written = 0;
    const std::uint64_t now = GetTickCount64();
    // The burst runs only while the client is loading. A join or a transition-token change opens
    // that window. Outside it the roster goes out on the keepalive alone.
    const bool burstDue =
        now < session.activityTransitionUntilTick && now >= session.activityRosterDueTick;
    const bool keepaliveDue = now >= session.activityKeepaliveDueTick;
    // A region change cannot wait for the keepalive. The client claims the next region almost at
    // once. Only the reported field is read here, because this runs on every pump.
    const bool active = session.activity.role != ActivityClientRole::none
                        && state::activity::binding_matches(session.activity.session)
                        && state::activity::binding_matches(session.activity.source);
    const bool isPrivate = session.activity.role == ActivityClientRole::privateCurrent;
    const std::int32_t reportedRegion =
        active && isPrivate
            ? state::activity::membership::reported_region(session.activity.source.sessionId)
            : -1;
    const bool regionChanged =
        isPrivate && reportedRegion >= 0 && reportedRegion != session.activity.advertisedRegion;
    if (!active || (!burstDue && !keepaliveDue && !regionChanged)) {
        return false;
    }
    touchesScratch = true;

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    const auto& key = state::bap().sessionKey;
    bool published = false;
    // Held until the frame reaches the caller. Encoding alone does not spend the region trigger.
    std::int32_t stagedAdvertisedRegion = -1;
    // The burst is what step 36 waits on. When both timers fire together the roster goes out last,
    // because the type-13 key binds to the player message 12 creates.
    if (!keepaliveDue && !regionChanged) {
        published = append_roster_notification(
            session, scratch, key, nextSendNonce, scratch.framed, framedSize, true);
        const bool delivered = publish_frame(
            session, scratch, response, written, framedSize, nextSendNonce, published);
        if (delivered) {
            session.activityRosterDueTick = now + kRosterBurstIntervalMs;
        }
        return delivered;
    }
    published = append_global_state_notification(
        scratch, session.activity.session, key, nextSendNonce, scratch.framed, framedSize);
    // Nothing may advance membership State until the first required frame proves this caller owns
    // enough capacity to publish at least the keepalive prefix.
    if (!published || framedSize > response.size()) {
        static_cast<void>(publish_frame(
            session, scratch, response, written, framedSize, nextSendNonce, published));
        return false;
    }
    if (session.activity.role == ActivityClientRole::publicTarget) {
        // The public target owns its own epoch and roster. It must not advertise another target,
        // and the citizen advertisement inside the body is already gated on `privateCurrent`.
        //
        // It still owes one membership body. The client's msg 12 handler is the only writer of the
        // flag that binds a world container to this ActivityClient, and until that bind lands the
        // entity-slot grant sent at join has no view to reach:
        // `RE/31 "A grant reaches a view only through a bound world container"`.
        //
        // Exactly one body per binding. The flag the client sets is one-way, it never acknowledges
        // one on this link, and a link that joined a session it did not allocate never reports a
        // region -- so neither an acknowledgement nor a revision gate can ever close.
        state::activity::membership::PendingMutation staged{};
        bool appended = false;
        const bool owesMembership =
            core::settings::get().server.activation.activityPublicMembership
            && session.activityMembershipSentGeneration != session.activity.bindingGeneration;
        if (owesMembership) {
            // The body is the private link's member table, sent verbatim on this envelope.
            //
            // This link can author nothing of its own. Its join names the host rather than itself,
            // so its member key is the host's id, and the client never sends its identity here so
            // its own record stays empty. The client matches itself by the key at `client+27696`,
            // which is its machine id and is the same on both links, so only the private snapshot
            // carries a member the client recognises as the local player. A body carrying anything
            // else makes the client prune the member and destroy the player it holds.
            //
            // Read, never committed. `prepare_refresh` captures without changing State, and this
            // link must not move the private session's membership revision.
            const std::uint64_t privateSessionId =
                state::activity::membership::live_region_session(state::activity::kAbsentSessionId);
            // Zero until the client publishes its identity. Before that the private table still
            // holds the seed's placeholder, which is not what the client matches on either.
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
        published = append_roster_notification(
                        session, scratch, key, nextSendNonce, scratch.framed, framedSize, false)
                    || published;
        SecureZeroMemory(&staged, sizeof staged);
        const bool delivered = publish_frame(
            session, scratch, response, written, framedSize, nextSendNonce, published);
        if (delivered) {
            // Latched here, not at encode. An encoded body the client never saw is not a send.
            if (appended) {
                session.activityMembershipSentGeneration = session.activity.bindingGeneration;
            }
            session.activityKeepaliveDueTick = now + kActivityKeepaliveIntervalMs;
            session.activityRosterDueTick = now + kRosterBurstIntervalMs;
        }
        return delivered;
    }

    // The client applies one membership update per revision and drops repeats, so an already
    // acknowledged region change needs a new revision to land. Move it only when there is a real
    // advertisement, or an empty channel advances the revision on every poll.
    // Membership only becomes publishable after the client sends its identity, so it joins the
    // keepalive instead of the join reply.
    state::activity::membership::PendingMutation refresh{};
    state::activity::membership::PendingMutation stagedMembership{};
    const bool needsRepublish =
        regionChanged
        && server::gameplay::advertisement_state(session.activity.source, reportedRegion)
               == server::gameplay::AdvertisementState::ready
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
    const bool publishesMembership =
        hasMembership
        && (regionChanged
            || !state::activity::membership::acknowledged(session.activity.session.sessionId));
    // Resolved the way the body resolves it, not from the reported field. Before the first report
    // the arrival slice set stands in, and that first push carries a descriptor too.
    const server::gameplay::AdvertisementState advertisement =
        publishesMembership
            ? server::gameplay::advertisement_state(session.activity.source,
                                                    effective_region(session.activity.source).index)
            : server::gameplay::AdvertisementState::absent;
    bool appendedMembership = false;
    if (publishesMembership && advertisement == server::gameplay::AdvertisementState::pending) {
        // The client applies one membership update per revision, so a push made while the
        // advertisement is still being allocated spends that revision on a region record with no
        // join descriptor. The allocation lands in the next service slice, so holding costs a poll.
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         "ev=gameplay stage=membership result=held reason=no_host_session");
    } else if (publishesMembership) {
        activity_message::ActivityPlan plan{};
        plan.sessionId = session.activity.session.sessionId;
        plan.membershipMutation = refresh;
        const bool sent = append_membership_notification(
            scratch, session, plan, key, nextSendNonce, scratch.framed, framedSize);
        appendedMembership = sent;
        // Only `pending` leaves the trigger armed, because only `pending` is transient. `absent`
        // means this channel advertises nothing, so re-arming there republishes on every poll.
        if (sent && reportedRegion >= 0) {
            stagedAdvertisedRegion = reportedRegion;
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
    // The keepalive always carries the roster, in or out of a transition.
    published = append_roster_notification(
                    session, scratch, key, nextSendNonce, scratch.framed, framedSize, false)
                || published;

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
        return false;
    }
    SecureZeroMemory(&stagedMembership, sizeof stagedMembership);
    const bool delivered =
        publish_frame(session, scratch, response, written, framedSize, nextSendNonce, published);
    // A body the client never saw must advertise its region again on the next poll.
    if (delivered && stagedAdvertisedRegion >= 0) {
        session.activity.advertisedRegion = stagedAdvertisedRegion;
    }
    if (delivered) {
        // A held membership body is owed again soon, but not on every pump.
        session.activityKeepaliveDueTick =
            now + (deferredMembership ? kMembershipRetryIntervalMs : kActivityKeepaliveIntervalMs);
        session.activityRosterDueTick = now + kRosterBurstIntervalMs;
        if (preparedRepublish && appendedMembership) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             "ev=gameplay stage=membership result=republished reason=region");
        }
    }
    return delivered;
}

} // namespace sunrise::server::bap::encrypted::push::activity
