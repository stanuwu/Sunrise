#include "activity_message_push.h"

#include <Windows.h>

#include <algorithm>

#include "../../../../../middleware/bap/activity_message/activity_join_result_encoder.h"
#include "../../../../../middleware/bap/activity_message/bubble_host_state.h"
#include "../../../../../middleware/bap/activity_message/entity_slots.h"
#include "../../../../../middleware/secure_channel/runtime.h"
#include "../../../../gameplay/gameplay_advertisement.h"
#include "activity_arrival.h"
#include "activity_global_state_push.h"
#include "activity_membership_push.h"
#include "activity_notification_frame.h"
#include "activity_world_globals_push.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {

namespace service = middleware::bap::activity_message;

/** Activity message type 4 accepts a pending join before any later push. */
constexpr std::uint32_t kJoinResultMessageType = 4;

/**
 * Wipes the part of one scratch buffer that may hold written bytes.
 * @param buffer Lock-owned scratch storage.
 * @param size Largest prefix that may hold transformed bytes.
 */
void clear_prefix(std::span<std::byte> buffer, std::size_t size) noexcept {
    SecureZeroMemory(buffer.data(), (std::min)(buffer.size(), size));
}

} // namespace

/** Appends the whole svc9 burst one join is answered with. */
bool append_join_notifications(Scratch& scratch,
                               Session& session,
                               const activity_message::ActivityPlan& activity,
                               std::span<const std::byte, state::kAesKeySize> key,
                               std::array<std::byte, state::kBapNonceSize>& nonce,
                               std::span<std::byte> response,
                               std::size_t& written) noexcept {
    if (written > response.size()) {
        return false;
    }
    session.activityJoinMembershipStaged = false;
    const std::size_t initialWritten = written;
    auto initialNonce = nonce;
    std::size_t messageSize = 0;
    bool encoded = service::join_result::encode_join_result(
                       activity.correlation,
                       activity.sessionId,
                       kLocalPeerHeardWindowMilliseconds,
                       kLocalKeepaliveHintMilliseconds,
                       static_cast<std::uint8_t>(activity.entitySlotMutation.replicationSequence),
                       scratch.responseBody,
                       messageSize)
                   && append_notification_frame(scratch,
                                                activity.sessionId,
                                                kJoinResultMessageType,
                                                std::span(scratch.responseBody).first(messageSize),
                                                key,
                                                nonce,
                                                response,
                                                written);
    clear_prefix(scratch.responseBody, messageSize);
    if (encoded) {
        middleware::secure_channel::advance_nonce(nonce);
        encoded = append_entity_slot_notification(scratch,
                                                  activity.sessionId,
                                                  activity.entitySlotMutation.mask,
                                                  key,
                                                  nonce,
                                                  response,
                                                  written);
    }
    // Order is fixed. 4 goes first, the only message the router's pre-join arm accepts. Then 1,
    // because step 33 reads the activity name from it. Then 2, to enable the clock the client
    // freezes without. Then 54, to close the set with its empty host table.
    if (encoded) {
        encoded = append_global_state_notification(
                      scratch, activity.targetBinding, key, nonce, response, written)
                  && append_world_globals_notification(
                      scratch, activity.sessionId, key, nonce, response, written);
    }
    if (encoded) {
        const service::bubble_host_state::Table bubbleHosts{};
        messageSize = 0;
        encoded = service::bubble_host_state::encode(bubbleHosts, scratch.responseBody, messageSize)
                  && append_notification_frame(scratch,
                                               activity.sessionId,
                                               service::bubble_host_state::kMessageType,
                                               std::span(scratch.responseBody).first(messageSize),
                                               key,
                                               nonce,
                                               response,
                                               written);
        clear_prefix(scratch.responseBody, messageSize);
        if (encoded) {
            middleware::secure_channel::advance_nonce(nonce);
        }
    }
    // Retail answers a join with one burst: result, grant mask, then membership. A private
    // link's body carries the join descriptor, so it is held only while that row is missing.
    if (encoded && activity.membershipMutation.hasSnapshot) {
        if (activity.bindingIntent == activity_message::BindingIntent::publicTarget
            || activity.bindingIntent == activity_message::BindingIntent::sharedTarget) {
            encoded = append_join_membership_notification(
                scratch, session, activity, key, nonce, response, written);
            session.activityJoinMembershipStaged = encoded;
        } else if (activity.bindingIntent == activity_message::BindingIntent::preserveCurrent
                   && region_advertisement(session, effective_region(session.activity.source).index)
                          != server::gameplay::AdvertisementState::pending) {
            encoded = append_membership_notification(
                scratch, session, activity, key, nonce, response, written);
            session.activityJoinMembershipStaged = encoded;
        }
    }
    if (!encoded) {
        session.activityJoinMembershipStaged = false;
        // Never show a first notification when the one that must follow cannot be staged.
        clear_prefix(response.subspan(initialWritten), written - initialWritten);
        written = initialWritten;
        nonce = initialNonce;
    }
    SecureZeroMemory(&initialNonce, sizeof initialNonce);
    return encoded;
}

/** Appends one entity-slot svc9 notification and advances its local nonce once. */
bool append_entity_slot_notification(Scratch& scratch,
                                     std::uint64_t sessionId,
                                     std::span<const std::byte> entitySlots,
                                     std::span<const std::byte, state::kAesKeySize> key,
                                     std::array<std::byte, state::kBapNonceSize>& nonce,
                                     std::span<std::byte> response,
                                     std::size_t& written) noexcept {
    if (written > response.size() || entitySlots.size() != service::entity_slots::kEncodedSize) {
        return false;
    }

    const std::size_t initialWritten = written;
    auto initialNonce = nonce;
    std::size_t messageSize = 0;
    const std::span<const std::byte, service::entity_slots::kEncodedSize> selected{
        entitySlots.data(), entitySlots.size()};
    const bool encoded =
        service::entity_slots::encode_entity_slots(selected, scratch.responseBody, messageSize)
        && append_notification_frame(scratch,
                                     sessionId,
                                     service::entity_slots::kNotificationMessageType,
                                     std::span(scratch.responseBody).first(messageSize),
                                     key,
                                     nonce,
                                     response,
                                     written);
    clear_prefix(scratch.responseBody, messageSize);
    if (encoded) {
        middleware::secure_channel::advance_nonce(nonce);
    } else {
        clear_prefix(response.subspan(initialWritten), written - initialWritten);
        written = initialWritten;
        nonce = initialNonce;
    }
    SecureZeroMemory(&initialNonce, sizeof initialNonce);
    return encoded;
}

} // namespace sunrise::server::bap::encrypted::push::activity
