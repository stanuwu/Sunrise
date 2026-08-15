#include "activity_transaction_notifications.h"

#include "../push/activity/activity_global_state_push.h"
#include "../push/activity/activity_membership_push.h"
#include "../push/activity/activity_message_push.h"
#include "../push/activity/activity_roster_push.h"

namespace sunrise::server::bap::encrypted::activity_transaction {
namespace {

/**
 * Stages the whole host snapshot the client's state-refresh request asks for.
 * The order matches the keepalive: the global state, then membership, then the roster, because the
 * roster's participation key binds to the player the membership publishes.
 * @param session Connection-owned roster counters, advanced only by a staged roster.
 * @param scratch Lock-owned transform buffers.
 * @param activity Prepared activity transaction carrying the membership snapshot.
 * @param key Active AES-GCM session key.
 * @param nonce Local send nonce advanced only by complete staged notifications.
 * @param response Lock-owned complete-frame staging storage.
 * @param written Existing staged byte count, updated only by complete notifications.
 * @return True when at least one of the three notifications was staged.
 */
[[nodiscard]] bool stage_refresh(Session& session,
                                 Scratch& scratch,
                                 const activity_message::ActivityPlan& activity,
                                 std::span<const std::byte, state::kAesKeySize> key,
                                 std::array<std::byte, state::kBapNonceSize>& nonce,
                                 std::span<std::byte> response,
                                 std::size_t& written) noexcept {
    bool staged = push::activity::append_global_state_notification(
        scratch, activity.sessionId, key, nonce, response, written);
    if (activity.membershipMutation.hasSnapshot) {
        staged = push::activity::append_membership_notification(
                     scratch, activity, key, nonce, response, written)
                 || staged;
    }
    return push::activity::append_roster_notification(
               session, scratch, key, nonce, response, written, false)
           || staged;
}

/**
 * Stages what one client-authoritative delta owes: membership, the roster, or both.
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
                                       std::size_t& written) noexcept {
    bool staged = false;
    if (activity.membershipMutation.hasSnapshot) {
        staged = push::activity::append_membership_notification(
            scratch, activity, key, nonce, response, written);
    }
    if (activity.regionMoved) {
        staged = push::activity::append_roster_notification(
                     session, scratch, key, nonce, response, written, false)
                 || staged;
    }
    return staged;
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
                         std::size_t& written) noexcept {
    // Each encoder refuses an absent session itself, so a plan that delivers nothing needs no
    // session at all. Message type 52 is the one that arrives on an unallocated link.
    if (activity.delivery == activity_message::Delivery::joinNotifications) {
        return push::activity::append_join_notifications(
            scratch, activity, key, nonce, response, written);
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
        return push::activity::append_membership_notification(
            scratch, activity, key, nonce, response, written);
    }
    if (activity.delivery == activity_message::Delivery::refreshNotifications) {
        return stage_refresh(session, scratch, activity, key, nonce, response, written);
    }
    if (activity.delivery == activity_message::Delivery::authoritativeNotifications) {
        return stage_authoritative(session, scratch, activity, key, nonce, response, written);
    }
    return activity.delivery == activity_message::Delivery::none;
}

} // namespace sunrise::server::bap::encrypted::activity_transaction
