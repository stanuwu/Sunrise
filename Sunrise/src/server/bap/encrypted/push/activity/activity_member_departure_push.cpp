#include "activity_member_departure_push.h"

#include <Windows.h>

#include <algorithm>

#include "../../../../../middleware/bap/activity_message/activity_host_control.h"
#include "../../../../../middleware/bap/activity_message/activity_join_result_encoder.h"
#include "../../../../../middleware/secure_channel/runtime.h"
#include "../../../../../state/activity/member_departure.h"
#include "activity_notification_frame.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {
/** Single-bubble selector, the value the cleaned reference puts in every departure purge. */
constexpr std::uint8_t kDepartureBubbleSelector = 0;
/**
 * How long a changed member set waits for this recipient's membership receipt before the rejoin
 * replay goes out unacknowledged. The fork's own bound; without one a client that never
 * acknowledges never receives its rejoin.
 */
constexpr std::uint64_t kRejoinAcknowledgementWaitMs = 20'000;
} // namespace

bool consume_member_rejoin(Session& session,
                           Scratch& scratch,
                           std::span<std::byte> response,
                           std::size_t& written,
                           bool& touchesScratch) noexcept {
    written = 0;
    if (!session.authenticated || !session.activityMemberKey || !session.activityJoinGeneration
        || session.activityJoinGeneration != session.activity.bindingGeneration
        || !session.activityJoinCorrelation || session.activityRejoinSends >= 8) {
        return false;
    }
    state::activity::JoinedMemberSet members{};
    if (!state::activity::joined_member_set(
            session.activity.session, session.activityMemberKey, members)
        || members.keys == session.activityMemberSet) {
        session.activityRejoinMemberSet = {};
        session.activityRejoinDeadlineTick = 0;
        return false;
    }
    // A changed set owes one replay of this recipient's original native correlation. The
    // membership receipt is the ordering barrier this replay wants, but it is not a precondition
    // for owing it: the fork waits `kRejoinAcknowledgementWaitMs` for that receipt and then sends
    // anyway (`activity_rejoin_push.cpp:103-109` and `activity_rejoin_policy.h:9` in the cleaned
    // reference). A client that never acknowledges must not be left without its rejoin forever.
    const auto now = GetTickCount64();
    if (!session.activityRejoinDeadlineTick || session.activityRejoinMemberSet != members.keys) {
        session.activityRejoinMemberSet = members.keys;
        session.activityRejoinDeadlineTick = now + kRejoinAcknowledgementWaitMs;
    }
    // `JoinedMemberSet::acknowledged` reads the member row of the session this link is bound to,
    // shared by every link the member holds there. The receipt kept against the body this
    // connection delivered releases this link on its own answer.
    const bool acknowledged = members.acknowledged || connection_membership_acknowledged(session);
    if (!acknowledged && now < session.activityRejoinDeadlineTick) {
        return false;
    }
    namespace join = middleware::bap::activity_message::join_result;
    std::array<std::byte, join::kEncodedSize> bytes{};
    std::size_t bodySize{}, framedSize{};
    touchesScratch = true;
    const bool encoded = join::encode_join_result(session.activityJoinCorrelation,
                                                  session.activity.session.sessionId,
                                                  5000,
                                                  2000,
                                                  bytes,
                                                  bodySize)
                         && append_notification_frame(scratch,
                                                      session.activity.session.sessionId,
                                                      4,
                                                      std::span(bytes).first(bodySize),
                                                      session.sessionKey,
                                                      session.sendNonce,
                                                      scratch.framed,
                                                      framedSize)
                         && framedSize != 0 && framedSize <= response.size();
    if (!encoded) {
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    middleware::secure_channel::advance_nonce(session.sendNonce);
    session.activityMemberSet = members.keys;
    session.activityRejoinMemberSet = {};
    session.activityRejoinDeadlineTick = 0;
    ++session.activityRejoinSends;
    return true;
}

bool consume_member_departure(Session& session,
                              Scratch& scratch,
                              std::span<std::byte> response,
                              std::size_t& written,
                              bool& touchesScratch) noexcept {
    written = 0;
    if (!session.authenticated || !session.activityMemberKey || !session.activityJoinGeneration
        || session.activityJoinGeneration != session.activity.bindingGeneration) {
        return false;
    }
    state::activity::MemberPurge pending{};
    if (!state::activity::pending_member_purge(
            session.activity.session, session.activityMemberKey, pending)) {
        return false;
    }
    namespace control = middleware::bap::activity_message::host_control;
    // The message-25 byte between the reason and the mask is the bubble selector, and a departure
    // notification carries the single-bubble value zero -- what the cleaned reference sends
    // (`activity_rejoin_push.cpp:180-185`, `entity_slot_purge.h`'s `selector`) and what the
    // accepted `purge-polarity-f1` boot witnessed. It is not this link's replication epoch: that
    // counter belongs to the client-requested authority purge, whose senders and guards all carry
    // `replicationEpoch + 1` (`activity_message_framing.cpp:295`,
    // `activity_transaction_notifications.cpp:200-201`, `bap_route.cpp:676`) and commit it. A
    // departure purge joins no epoch handshake, so it must not spend an epoch value either.
    const control::PurgeAuthorityBody body{pending.slots, kDepartureBubbleSelector, 0};
    std::array<std::byte, control::kPurgeAuthorityByteCount> bytes{};
    std::size_t bodySize{}, framedSize{};
    auto nonce = session.sendNonce;
    touchesScratch = true;
    const bool encoded = control::encode_purge_authority(body, bytes, bodySize)
                         && append_notification_frame(scratch,
                                                      pending.binding.sessionId,
                                                      control::kPurgeAuthorityMessageType,
                                                      std::span(bytes).first(bodySize),
                                                      session.sessionKey,
                                                      nonce,
                                                      scratch.framed,
                                                      framedSize)
                         && framedSize != 0 && framedSize <= response.size();
    // A capacity refusal leaves both the native debt and the nonce untouched for the next pump.
    if (!encoded || !state::activity::commit_member_purge(pending)) {
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    middleware::secure_channel::advance_nonce(nonce);
    session.sendNonce = nonce;
    session.activityKeepaliveDueTick = 0;
    return true;
}
} // namespace sunrise::server::bap::encrypted::push::activity
