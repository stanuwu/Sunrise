#include "activity_keepalive_push.h"

#include <Windows.h>

#include <array>
#include <cstdio>

#include "../../../../../core/logging/log.h"
#include "../../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../../state/runtime/runtime.h"
#include "../../activity_message/definition.h"
#include "activity_global_state_push.h"
#include "activity_membership_push.h"
#include "activity_roster_push.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {

/**
 * Keepalive cadence. The client tears the session down after 20.5 seconds of server silence, so
 * 5 seconds leaves 3 writes of margin.
 */
constexpr std::uint64_t kKeepaliveIntervalMs = 5'000;
/**
 * Roster burst cadence, used only while the client is loading. Outside that window the roster
 * rides the keepalive.
 */
constexpr std::uint64_t kRosterBurstIntervalMs = 1'000;
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
        return false;
    }
    for (std::size_t index = 0; index < framedSize; ++index) {
        response[index] = scratch.framed[index];
    }
    written = framedSize;
    session.sendNonce = nextSendNonce;
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
    if (session.activitySessionId == 0 || (!burstDue && !keepaliveDue)) {
        return false;
    }
    touchesScratch = true;

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    const auto& key = state::bap().sessionKey;
    bool published = false;
    // The burst is what step 36 waits on. When both timers fire together the roster goes out last,
    // because the type-13 key binds to the player message 12 creates.
    if (!keepaliveDue) {
        session.activityRosterDueTick = now + kRosterBurstIntervalMs;
        published = append_roster_notification(
            session, scratch, key, nextSendNonce, scratch.framed, framedSize, true);
        return publish_frame(
            session, scratch, response, written, framedSize, nextSendNonce, published);
    }
    session.activityKeepaliveDueTick = now + kKeepaliveIntervalMs;
    published = append_global_state_notification(
        scratch, session.activitySessionId, key, nextSendNonce, scratch.framed, framedSize);

    // Membership only becomes publishable after the client sends its identity, so it joins the
    // keepalive instead of the join reply.
    state::activity::membership::PendingMutation refresh{};
    bool hasMembership = state::activity::membership::prepare_refresh(
                             session.activitySessionId, kCurrentRevision, kNoBubble, refresh)
                         && refresh.hasSnapshot;
    if (!hasMembership
        && seed_identity(
            session.activitySessionId, session.activityMemberKey, session.activityCharacterSoid)) {
        SecureZeroMemory(&refresh, sizeof refresh);
        hasMembership = state::activity::membership::prepare_refresh(
                            session.activitySessionId, kCurrentRevision, kNoBubble, refresh)
                        && refresh.hasSnapshot;
    }
    // A client value wins; this only fills the token when nothing has published one.
    if (hasMembership && refresh.snapshot.transitionToken == 0
        && seed_transition_token(session.activitySessionId)) {
        SecureZeroMemory(&refresh, sizeof refresh);
        hasMembership = state::activity::membership::prepare_refresh(
                            session.activitySessionId, kCurrentRevision, kNoBubble, refresh)
                        && refresh.hasSnapshot;
    }
    // An acknowledged snapshot is stable. Re-sending it forces the client to re-tabulate its
    // region table and rebuild every player snapshot.
    if (hasMembership && !state::activity::membership::acknowledged(session.activitySessionId)) {
        activity_message::ActivityPlan plan{};
        plan.sessionId = session.activitySessionId;
        plan.membershipMutation = refresh;
        published = append_membership_notification(
                        scratch, plan, key, nextSendNonce, scratch.framed, framedSize)
                    || published;
        SecureZeroMemory(&plan, sizeof plan);
    }
    // The mirrored host state is captured before the secure clear, because the report below shows
    // whether the client asked for a state this host must not publish.
    const int reportedSpawnState = static_cast<int>(refresh.snapshot.spawn.state);
    const int reportedTeleportState = static_cast<int>(refresh.snapshot.teleport.state);
    const std::int32_t reportedTeleportSlice = refresh.snapshot.teleport.sliceSetIndex;
    const std::uint8_t reportedToken = refresh.snapshot.transitionToken;
    SecureZeroMemory(&refresh, sizeof refresh);
    // The keepalive always carries the roster, in or out of a transition.
    session.activityRosterDueTick = now + kRosterBurstIntervalMs;
    published = append_roster_notification(
                    session, scratch, key, nextSendNonce, scratch.framed, framedSize, false)
                || published;

    std::array<char, core::log::kLineCapacity> line{};
    const int count =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=activity stage=keepalive result=%s bytes=%zu membership=%u key=0x%llX "
                      "spawn_state=%d teleport_state=%d teleport_slice=%d token=%u",
                      published ? "ok" : "fail",
                      framedSize,
                      hasMembership ? 1U : 0U,
                      static_cast<unsigned long long>(session.activityMemberKey),
                      reportedSpawnState,
                      reportedTeleportState,
                      reportedTeleportSlice,
                      static_cast<unsigned>(reportedToken));
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(count)});
    }
    return publish_frame(session, scratch, response, written, framedSize, nextSendNonce, published);
}

} // namespace sunrise::server::bap::encrypted::push::activity
