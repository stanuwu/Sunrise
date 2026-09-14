#include "activity_transaction_notifications.h"

#include "../../../../middleware/secure_channel/runtime.h"
#include "../../../gameplay/gameplay_advertisement.h"
#include "../bap_connection_publication.h"
#include "../push/activity/activity_arrival.h"
#include "../push/activity/activity_global_state_push.h"
#include "../push/activity/activity_membership_push.h"
#include "../push/activity/activity_message_push.h"
#include "../push/activity/activity_notification_frame.h"
#include "../push/activity/activity_roster_push.h"
#include "../push/activity/activity_world_globals_push.h"
#include "../push/activity/internal.h"

namespace sunrise::server::bap::encrypted::activity_transaction {
namespace {

/**
 * Reports whether the citizen advertisement this membership body would carry is still coming.
 * One membership update lands per revision, so a body sent before the region's host session exists
 * spends that revision on a record no later push can fill. Holding costs one keepalive.
 * @param activity Prepared activity transaction, whose region this body publishes.
 * @return True when the push has to wait.
 */
[[nodiscard]] bool advertisement_pending(const Session& session,
                                         const activity_message::ActivityPlan& activity) noexcept {
    if (session.activity.role != ActivityClientRole::privateCurrent
        || !state::activity::binding_matches(session.activity.source)) {
        return false;
    }
    // Take the delta's region, not the committed one. Staging runs before the commit, so the
    // committed value still names the region the player has left.
    const push::activity::EffectiveRegion region = push::activity::private_planned_region(
        activity.membershipMutation, session.activity.source);
    const server::gameplay::AdvertisementState state =
        push::activity::region_advertisement(session, region.index);
    if (state != server::gameplay::AdvertisementState::pending) {
        return false;
    }
    return true;
}

/**
 * Stages the whole host snapshot the client's state-refresh request asks for.
 * The order matches the join burst: the global state and its clock enable, then membership, then
 * the roster. The roster's participation key binds to the player the membership publishes.
 * @param session Connection-owned roster counters, advanced only by a staged roster.
 * @param scratch Lock-owned transform buffers.
 * @param activity Prepared activity transaction carrying the membership snapshot.
 * @param key Active AES-GCM session key.
 * @param nonce Local send nonce advanced only by complete staged notifications.
 * @param response Lock-owned complete-frame staging storage.
 * @param written Existing staged byte count, updated only by complete notifications.
 * @return True only when the complete requested snapshot was staged.
 */
[[nodiscard]] bool stage_refresh(Session& session,
                                 Scratch& scratch,
                                 const activity_message::ActivityPlan& activity,
                                 std::span<const std::byte, state::kAesKeySize> key,
                                 std::array<std::byte, state::kBapNonceSize>& nonce,
                                 std::span<std::byte> response,
                                 std::size_t& written,
                                 bool allowEntityRetirement) noexcept {
    const auto initialNonce = nonce;
    const auto initialWritten = written;
    const bool initialRosterDebt = session.activityRosterOwedForEpoch;
    const bool needsMembership = session.activity.role == ActivityClientRole::privateCurrent;
    const bool membershipReady =
        !needsMembership
        || (activity.membershipMutation.hasSnapshot && !advertisement_pending(session, activity));
    const push::activity::RefreshReport refresh{activity.membershipMutation.bubbleIndex,
                                                activity.membershipMutation.requestedRevision};
    const bool complete =
        membershipReady
        && push::activity::append_global_state_notification(
            scratch, session.activity.session, key, nonce, response, written)
        && push::activity::append_world_globals_notification(
            scratch, session.activity.session.sessionId, key, nonce, response, written)
        && (!needsMembership
            || push::activity::append_membership_notification(
                scratch, session, activity, key, nonce, response, written))
        && push::activity::append_roster_notification(session,
                                                      scratch,
                                                      key,
                                                      nonce,
                                                      response,
                                                      written,
                                                      nullptr,
                                                      nullptr,
                                                      true,
                                                      &refresh,
                                                      allowEntityRetirement);
    if (!complete) {
        push::activity::discard_staged_roster(session);
        discard_staged_advertisement(session);
        session.activityRosterOwedForEpoch = initialRosterDebt;
        nonce = initialNonce;
        written = initialWritten;
    }
    return complete;
}

/**
 * Stages what one client-reported, host-committed delta owes: membership, the roster, or both.
 *
 * The roster follows membership, because its participation key binds to the player membership
 * publishes.
 * @param session Connection-owned roster counters, advanced only by a staged roster.
 * @param scratch Lock-owned transform buffers.
 * @param activity Prepared activity transaction and its region-move flag.
 * @param key Active AES-GCM session key.
 * @param nonce Local send nonce advanced only by complete staged notifications.
 * @param response Lock-owned complete-frame staging storage.
 * @param written Existing staged byte count, updated only by complete notifications.
 * @return True when every notification the delta owed was staged.
 */
[[nodiscard]] bool stage_authoritative(Session& session,
                                       Scratch& scratch,
                                       const activity_message::ActivityPlan& activity,
                                       std::span<const std::byte, state::kAesKeySize> key,
                                       std::array<std::byte, state::kBapNonceSize>& nonce,
                                       std::span<std::byte> response,
                                       std::size_t& written,
                                       bool allowEntityRetirement) noexcept {
    bool staged = false;
    bool stagedMembership = false;
    bool held = false;
    bool owed = false;
    bool suppressed = false;
    if (session.activity.role == ActivityClientRole::privateCurrent
        && activity.membershipMutation.hasSnapshot) {
        owed = true;
        held = advertisement_pending(session, activity);
        if (!held) {
            // A delta that changed the published fields never matches the delivered body, so
            // only a true repeat is skipped here.
            stagedMembership = push::activity::append_membership_notification(
                scratch, session, activity, key, nonce, response, written, true, &suppressed);
            staged = stagedMembership;
        }
    }
    if (session.activity.role == ActivityClientRole::privateCurrent && activity.regionMoved) {
        owed = true;
        // This notification is encoded before the membership transaction commits. Its roster
        // must use the prepared move, never the old committed msg-22 region.
        const push::activity::EffectiveRegion region =
            push::activity::planned_region(activity.membershipMutation, session.activity.source);
        // Staged before the commit, so readiness reads the incoming leg too. Without it the
        // body repeats the loading lifetime this very report says is over.
        push::activity::RefreshReport report{};
        report.currentRegion = activity.membershipMutation.authoritativeInput.currentRegion.index;
        report.hasCurrentRegion = activity.membershipMutation.authoritativeInput.hasCurrentRegion;
        // The client reports the region it now holds once its slice set is instantiated, and the
        // roster is that report's answer. It is solicited, so it is never skipped as a repeat,
        // including while the slice set is still instantiating.
        staged = push::activity::append_roster_notification(session,
                                                            scratch,
                                                            key,
                                                            nonce,
                                                            response,
                                                            written,
                                                            nullptr,
                                                            &region,
                                                            true,
                                                            &report,
                                                            allowEntityRetirement)
                 || staged;
    }
    // A delta that owed nothing is not a failure, and a public link never owes the block above.
    // Held and already-delivered bodies are not failures either: a false here would drop the
    // very commit the body reflects.
    return !owed || staged || held || suppressed;
}

} // namespace

/**
 * Stages the notifications one activity transaction requests.
 * @param session Connection-owned roster counters, advanced only by a staged roster.
 * @param scratch Lock-owned transform buffers.
 * @param activity Prepared activity transaction and delivery selection.
 * @param key Active AES-GCM session key.
 * @param nonce Local send nonce advanced only by complete staged notifications.
 * @param response Lock-owned complete-frame staging storage.
 * @param written Existing staged byte count, updated only by complete notifications.
 * @return True when every requested notification is staged.
 */
bool stage_notifications(Session& session,
                         Scratch& scratch,
                         const activity_message::ActivityPlan& activity,
                         std::span<const std::byte, state::kAesKeySize> key,
                         std::array<std::byte, state::kBapNonceSize>& nonce,
                         std::span<std::byte> response,
                         std::size_t& written,
                         bool allowEntityRetirement) noexcept {
    // Each encoder refuses an absent session itself, so a plan that delivers nothing needs no
    // session at all. Message type 52 is the one that arrives on an unallocated link.
    if (activity.delivery == activity_message::Delivery::joinNotifications) {
        return push::activity::append_join_notifications(
            scratch, session, activity, key, nonce, response, written);
    }
    if (activity.delivery == activity_message::Delivery::purgeNotification) {
        namespace control = middleware::bap::activity_message::host_control;
        const auto& purge = activity.authorityPurge;
        if (!purge.pending || purge.sourceGeneration != session.activity.bindingGeneration
            || activity.sessionId != session.activity.session.sessionId
            || activity_link_count_locked(session.activity.session,
                                          session.activity.bindingGeneration)
                   != 1
            || purge.body.epoch
                   != static_cast<std::uint8_t>(session.activity.replicationEpoch + 1U)) {
            return false;
        }
        std::array<std::byte, control::kPurgeAuthorityByteCount> body{};
        std::size_t bodySize = 0;
        if (!control::encode_purge_authority(purge.body, body, bodySize)
            || !push::activity::append_notification_frame(scratch,
                                                          activity.sessionId,
                                                          control::kPurgeAuthorityMessageType,
                                                          std::span(body).first(bodySize),
                                                          key,
                                                          nonce,
                                                          response,
                                                          written)) {
            return false;
        }
        middleware::secure_channel::advance_nonce(nonce);
        return true;
    }
    if (activity.delivery == activity_message::Delivery::rosterNotification) {
        // The client's inbound dispatch table has no entry for a type-52 response, so that
        // message is a one-way report and owes nothing on its own.
        if (!session.activityRosterOwedForEpoch) {
            return true;
        }
        // The epoch it reports is what an earlier answer was missing, so that answer goes now. The
        // epoch comes from this message; the connection's own copy is published after this runs.
        return push::activity::append_roster_notification(session,
                                                          scratch,
                                                          key,
                                                          nonce,
                                                          response,
                                                          written,
                                                          &activity.patchEpoch,
                                                          nullptr,
                                                          true,
                                                          nullptr,
                                                          allowEntityRetirement);
    }
    if (activity.delivery == activity_message::Delivery::leaveNotification) {
        // The client waits in shutting_down with its slice set current, so region readiness is not
        // a gate here. Answering the report is what unregisters the rows it still holds.
        return push::activity::append_roster_notification(session,
                                                          scratch,
                                                          key,
                                                          nonce,
                                                          response,
                                                          written,
                                                          nullptr,
                                                          nullptr,
                                                          true,
                                                          nullptr,
                                                          false,
                                                          true);
    }
    if (activity.delivery == activity_message::Delivery::entitySlotNotification) {
        return push::activity::append_entity_slot_notification(scratch,
                                                               activity.sessionId,
                                                               activity.entitySlotMutation.mask,
                                                               key,
                                                               nonce,
                                                               response,
                                                               written);
    }
    if (activity.delivery == activity_message::Delivery::membershipNotification) {
        // A public target gets exactly one membership body, in its join burst. A second one here
        // would land mid-transition and would set no one-shot latch.
        if (session.activity.role == ActivityClientRole::publicTarget) {
            return true;
        }
        return push::activity::append_membership_notification(
            scratch, session, activity, key, nonce, response, written);
    }
    if (activity.delivery == activity_message::Delivery::refreshNotifications) {
        return stage_refresh(
            session, scratch, activity, key, nonce, response, written, allowEntityRetirement);
    }
    if (activity.delivery == activity_message::Delivery::authoritativeNotifications) {
        return stage_authoritative(
            session, scratch, activity, key, nonce, response, written, allowEntityRetirement);
    }
    return activity.delivery == activity_message::Delivery::none;
}

} // namespace sunrise::server::bap::encrypted::activity_transaction
