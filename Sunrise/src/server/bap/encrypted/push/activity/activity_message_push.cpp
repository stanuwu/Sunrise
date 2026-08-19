#include "activity_message_push.h"

#include <Windows.h>

#include <algorithm>

#include "../../../../../middleware/bap/activity_message/activity_join_result_encoder.h"
#include "../../../../../middleware/bap/activity_message/entity_slots.h"
#include "../../../../../middleware/secure_channel/runtime.h"
#include "activity_global_state_push.h"
#include "activity_notification_frame.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {

namespace service = middleware::bap::activity_message;

/** Activity message type 4 accepts a pending join before any later push. */
constexpr std::uint32_t kJoinResultMessageType = 4;
/** Activity message type 54 publishes the bubble-host table. */
constexpr std::uint32_t kBubbleHostStateMessageType = 54;
/**
 * Message 54 opens with a 6-bit host count, and a zero count carries no records, so the whole
 * encoded body is one zero byte.
 */
constexpr std::array<std::byte, 1> kEmptyBubbleHostState{};
/** 5 seconds stops the zero-hint keepalive flood seen locally. */
constexpr std::uint16_t kLocalKeepaliveHintMilliseconds = 5'000;

/**
 * Wipes the part of one scratch buffer that may hold written bytes.
 * @param buffer Lock-owned scratch storage.
 * @param size Largest prefix that may hold transformed bytes.
 */
void clear_prefix(std::span<std::byte> buffer, std::size_t size) noexcept {
    SecureZeroMemory(buffer.data(), (std::min)(buffer.size(), size));
}

} // namespace

/** Appends the ordered join-result and entity-slot svc9 notifications. */
bool append_join_notifications(Scratch& scratch,
                               const activity_message::ActivityPlan& activity,
                               std::span<const std::byte, state::kAesKeySize> key,
                               std::array<std::byte, state::kBapNonceSize>& nonce,
                               std::span<std::byte> response,
                               std::size_t& written) noexcept {
    if (written > response.size()) {
        return false;
    }
    const std::size_t initialWritten = written;
    auto initialNonce = nonce;
    std::size_t messageSize = 0;
    bool encoded = service::join_result::encode_join_result(activity.correlation,
                                                            activity.sessionId,
                                                            kLocalKeepaliveHintMilliseconds,
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
    // Message 1 comes after 0: 4 first because it is the only message the router's pre-join arm
    // accepts, and 1 after it because step 33 reads the activity name out of it. Message 54 closes
    // the set with its empty host table.
    if (encoded) {
        encoded = append_global_state_notification(
            scratch, activity.targetBinding, key, nonce, response, written);
    }
    if (encoded) {
        encoded = append_notification_frame(scratch,
                                            activity.sessionId,
                                            kBubbleHostStateMessageType,
                                            kEmptyBubbleHostState,
                                            key,
                                            nonce,
                                            response,
                                            written);
        if (encoded) {
            middleware::secure_channel::advance_nonce(nonce);
        }
    }
    if (!encoded) {
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
