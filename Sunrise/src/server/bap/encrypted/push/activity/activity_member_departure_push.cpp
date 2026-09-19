#include "activity_member_departure_push.h"

#include <algorithm>

#include "../../../../../middleware/bap/activity_message/activity_host_control.h"
#include "../../../../../middleware/bap/activity_message/activity_join_result_encoder.h"
#include "../../../../../middleware/bap/activity_message/activity_replication_epoch_encoder.h"
#include "../../../../../middleware/secure_channel/runtime.h"
#include "../../../../../state/activity/member_departure.h"
#include "../../../../gameplay/peer/peer_transport.h"
#include "activity_notification_frame.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {
bool append_replication_steps(Session& session,
                              Scratch& scratch,
                              std::uint64_t target,
                              const state::activity::MemberPurge* purge,
                              std::span<const std::byte, state::kAesKeySize> key,
                              std::array<std::byte, state::kBapNonceSize>& nonce,
                              std::span<std::byte> response,
                              std::size_t& written) noexcept {
    namespace control = middleware::bap::activity_message::host_control;
    namespace epoch = middleware::bap::activity_message::replication_epoch;
    if (target < session.activity.replicationSequence
        || (purge && purge->memberKey
            && (purge->replicationSequence <= session.activity.replicationSequence
                || purge->replicationSequence > target))) {
        return false;
    }
    for (auto sequence = session.activity.replicationSequence; sequence < target;) {
        ++sequence;
        std::array<std::byte, control::kPurgeAuthorityByteCount> bytes{};
        std::size_t size{};
        const bool hasPurge = purge && purge->memberKey && purge->replicationSequence == sequence;
        const bool encoded =
            hasPurge ? control::encode_purge_authority(
                           {purge->slots, static_cast<std::uint8_t>(sequence), 0}, bytes, size)
                     : epoch::encode(static_cast<std::uint8_t>(sequence), bytes, size);
        if (!encoded
            || !append_notification_frame(scratch,
                                          session.activity.session.sessionId,
                                          hasPurge ? control::kPurgeAuthorityMessageType
                                                   : epoch::kMessageType,
                                          std::span(bytes).first(size),
                                          key,
                                          nonce,
                                          response,
                                          written)) {
            return false;
        }
        middleware::secure_channel::advance_nonce(nonce);
    }
    return true;
}

std::size_t commit_replication_steps(Session& session, std::uint64_t target) noexcept {
    std::size_t updated{};
    while (session.activity.replicationSequence < target) {
        const auto previous = session.activity.replicationEpoch;
        ++session.activity.replicationSequence;
        session.activity.replicationEpoch =
            static_cast<std::uint8_t>(session.activity.replicationSequence);
        updated +=
            server::gameplay::peer::commit_replication_epoch(session.activity.session,
                                                             session.activity.bindingGeneration,
                                                             previous,
                                                             session.activity.replicationEpoch);
        auto& request = session.activityReplicationEpoch;
        if (request.bindingGeneration == session.activity.bindingGeneration
            && request.generation == session.activity.replicationEpoch) {
            request.pending = false;
            request.staged = false;
        }
    }
    return updated;
}

bool consume_replication_step(Session& session,
                              Scratch& scratch,
                              std::span<std::byte> response,
                              std::size_t& written,
                              bool& touchesScratch) noexcept {
    written = 0;
    std::uint64_t target{};
    if (!session.authenticated || !session.activityJoinGeneration
        || session.activityJoinGeneration != session.activity.bindingGeneration
        || !state::activity::replication_sequence(session.activity.session, target)
        || session.activity.replicationSequence >= target) {
        return false;
    }
    state::activity::MemberPurge pending{};
    if (state::activity::pending_member_purge(
            session.activity.session, session.activityMemberKey, pending)
        && pending.replicationSequence == session.activity.replicationSequence + 1) {
        return false;
    }
    auto nonce = session.sendNonce;
    std::size_t size{};
    touchesScratch = true;
    const auto next = session.activity.replicationSequence + 1;
    if (!append_replication_steps(
            session, scratch, next, nullptr, session.sessionKey, nonce, scratch.framed, size)
        || size > response.size()) {
        return false;
    }
    std::copy_n(scratch.framed.begin(), size, response.begin());
    written = size;
    session.sendNonce = nonce;
    commit_replication_steps(session, next);
    return true;
}

bool consume_member_rejoin(Session& session,
                           Scratch& scratch,
                           std::span<std::byte> response,
                           std::size_t& written,
                           bool& touchesScratch) noexcept {
    written = 0;
    if (!session.authenticated || !session.activityMemberKey || !session.activityJoinGeneration
        || session.activityJoinGeneration != session.activity.bindingGeneration
        || !session.activityJoinCorrelation) {
        return false;
    }
    std::uint64_t currentSequence{};
    if (!state::activity::replication_sequence(session.activity.session, currentSequence)
        || currentSequence != session.activity.replicationSequence) {
        return false;
    }
    state::activity::JoinedMemberSet members{};
    if (!state::activity::joined_member_set(
            session.activity.session, session.activityMemberKey, members)
        || members.keys == session.activityMemberSet) {
        return false;
    }
    // A changed set owes one replay of this recipient's original native correlation, and that
    // replay must not overtake the membership body naming the new set. On one ordered secure
    // channel the barrier is this link's own delivery cursor: while the keepalive still owes the
    // current revision here, the body has not been written into the stream yet. The purge forced
    // that body due, so the wait ends on the delivery rather than on the client's answer.
    if (connection_owes_membership(
            session,
            session.activity.session.sessionId,
            state::activity::membership::current_revision(session.activity.session))) {
        return false;
    }
    namespace join = middleware::bap::activity_message::join_result;
    std::array<std::byte, join::kEncodedSize> bytes{};
    std::size_t bodySize{}, framedSize{};
    touchesScratch = true;
    const bool encoded = join::encode_join_result(session.activityJoinCorrelation,
                                                  session.activity.session.sessionId,
                                                  kLocalPeerHeardWindowMilliseconds,
                                                  kLocalKeepaliveHintMilliseconds,
                                                  session.activity.replicationEpoch,
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
    // One replay per distinct member set: the latch is what bounds the replay rate to real
    // native joins and departures, so no send counter is needed.
    session.activityMemberSet = members.keys;
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
    // Native 16F0CE0 forwards this byte through world command 17 to 170B030, which
    // requires the next replication epoch for every purge, including a departure.
    const auto previousEpoch = session.activity.replicationEpoch;
    if (pending.replicationSequence != session.activity.replicationSequence + 1) {
        return false;
    }
    const auto nextEpoch = static_cast<std::uint8_t>(pending.replicationSequence);
    const control::PurgeAuthorityBody body{pending.slots, nextEpoch, 0};
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
    session.activity.replicationSequence = pending.replicationSequence;
    session.activity.replicationEpoch = nextEpoch;
    static_cast<void>(server::gameplay::peer::commit_replication_epoch(
        session.activity.session, session.activity.bindingGeneration, previousEpoch, nextEpoch));
    auto& request = session.activityReplicationEpoch;
    if (request.bindingGeneration == session.activity.bindingGeneration
        && request.generation == nextEpoch) {
        request.pending = false;
        request.staged = false;
    }
    session.activityKeepaliveDueTick = 0;
    return true;
}
} // namespace sunrise::server::bap::encrypted::push::activity
