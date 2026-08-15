#include "activity_message_route.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/bap/activity_message/activity_client_identity_parser.h"
#include "../../../../middleware/bap/activity_message/activity_client_keepalive_validator.h"
#include "../../../../middleware/bap/activity_message/activity_high_water_validator.h"
#include "../../../../middleware/bap/activity_message/activity_join_request_parser.h"
#include "../../../../middleware/bap/activity_message/activity_membership_acknowledgement_parser.h"
#include "../../../../middleware/bap/activity_message/activity_message_request_parser.h"
#include "../../../../middleware/bap/activity_message/activity_state_refresh_parser.h"
#include "../../../../middleware/bap/activity_message/client_authoritative_data.h"
#include "../../../../middleware/bap/activity_message/entity_authority.h"
#include "../../../../middleware/bap/activity_message/entity_slots.h"
#include "../../../../middleware/bap/activity_message/incident.h"
#include "../../../../state/activity/runtime.h"
#include "membership/activity_membership_route.h"
#include "middleware/bap/activity_message/activity_entity_slot_request_parser.h"
#include "patch_epoch/activity_patch_epoch_route.h"

namespace sunrise::server::bap::encrypted::activity_message {
namespace {

namespace service = middleware::bap::activity_message;
namespace authority = service::entity_authority;
namespace client_keepalive = service::client_keepalive;
namespace high_water = service::high_water;
namespace epoch_message = service::patch_epoch;

/** Activity message type 3 starts the client join transaction. */
constexpr std::uint32_t kJoinRequestMessageType = 3;

/** One row per Client-sent message this route accepts but has no state to change for. */
struct AcceptedMessage {
    std::uint32_t type;
    const char* name;
};

/**
 * The Client senders that carry no work for this host. Each is one-way, so accepting is the whole
 * contract. The names are the binary's own, so a log line says what arrived.
 */
constexpr std::array<AcceptedMessage, 14> kAcceptedMessages{{
    {6, "sensor_sense_update"},
    {8, "request_activity_host"},
    {11, "start_new_activity"},
    {13, "request_peer_reservation"},
    {14, "release_peer_reservation"},
    {15, "peer_leave_request"},
    {34, "process_debug_command"},
    {37, "connectivity_failure"},
    {39, "send_client_heartbeat"},
    {43, "bug_claw"},
    {46, "report_lag_switch"},
    {47, "connection_quality_report"},
    {48, "speculative_migration"},
    {50, "refresh_inspirations"},
}};

/** @return The binary name for one accepted message type, or nullptr when it is not one. */
[[nodiscard]] const char* accepted_name(std::uint32_t messageType) noexcept {
    const auto row = std::find_if(kAcceptedMessages.begin(),
                                  kAcceptedMessages.end(),
                                  [messageType](const AcceptedMessage& candidate) noexcept {
                                      return candidate.type == messageType;
                                  });
    return row == kAcceptedMessages.end() ? nullptr : row->name;
}

/**
 * Records one accepted message that changes no host state.
 * @param messageType Activity message type from the envelope.
 * @param name Binary name for that type.
 * @param payloadSize Declared payload bytes, which is the only thing that varies here.
 */
void report_accepted(std::uint32_t messageType,
                     const char* name,
                     std::size_t payloadSize) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=message result=accept type=%u name=%s "
                                      "bytes=%zu",
                                      messageType,
                                      name,
                                      payloadSize);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Checks one incident and reports its verdict. Nothing relays msg 19 yet, so a pass changes
 * nothing. A failure is named because a bad target index would crash the Client if it were sent on.
 * @param request Validated owned svc8 envelope.
 */
void report_incident(const service::Request& request) noexcept {
    namespace incident = service::incident;
    incident::Incident parsed;
    const incident::Verdict verdict = incident::validate(request.payload, parsed);
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=incident result=%s target=%u extra=%u "
                                      "selector=%u payload=%u",
                                      incident::verdict_name(verdict),
                                      parsed.primaryTarget,
                                      parsed.extraTargetCount,
                                      static_cast<unsigned>(parsed.hasCompressedSelector),
                                      parsed.payloadLength);
    if (written <= 0) {
        return;
    }
    const auto level =
        verdict == incident::Verdict::accepted ? core::log::Level::debug : core::log::Level::warn;
    core::log::write(
        core::log::Channel::server, level, {line.data(), static_cast<std::size_t>(written)});
}

/**
 * Reports one activity message the route did not stage, naming its type.
 * Every inbound activity message is one-way, so nothing here can jam the Client's reply ring. An
 * unnamed drop is invisible, and membership waits on the identity message.
 * @param messageType Activity message type from the envelope.
 * @param accountHandle Handle the envelope carried.
 * @param reason Short name of the step that declined.
 */
void report_message(std::uint32_t messageType,
                    std::uint64_t accountHandle,
                    const char* reason) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=message result=skip type=%u "
                                      "handle=0x%llX reason=%s",
                                      messageType,
                                      static_cast<unsigned long long>(accountHandle),
                                      reason);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Prepares the joined State and the whole initial lease mask as one mutation.
 * @param request Validated owned svc8 envelope.
 * @param plan Cleared, then receives join scalars and the chosen lease mask.
 * @return True when the fixed join payload and current State can stage together.
 */
[[nodiscard]] bool prepare_join(const service::Request& request, ActivityPlan& plan) noexcept {
    service::JoinRequest parsed;
    if (!service::join_request::parse_join_request(request.payload, parsed)
        || parsed.sessionId != request.accountHandle
        || !state::activity::entity_slots::prepare_join(parsed.sessionId,
                                                        parsed.memberKey,
                                                        state::activity::entity_slots::kSlotCount,
                                                        plan.entitySlotMutation)) {
        return false;
    }
    plan.correlation = parsed.correlation;
    plan.sessionId = parsed.sessionId;
    plan.joinCharacterSoid = parsed.characterSoid;
    plan.delivery = Delivery::joinNotifications;
    plan.mutationDomain = MutationDomain::entitySlots;
    return true;
}

/**
 * Prepares only currently free slots for one positive client request.
 * @param request Validated owned svc8 envelope.
 * @param plan Cleared, then receives the chosen lease mask.
 * @return True for a valid positive request, including an exhausted zero-mask grant.
 */
[[nodiscard]] bool prepare_grant(const service::Request& request, ActivityPlan& plan) noexcept {
    std::int32_t requested = 0;
    if (!service::entity_slot_request::parse_entity_slot_request(request.payload, requested)
        || requested <= 0
        || !state::activity::entity_slots::prepare_grant(
            request.accountHandle, static_cast<std::size_t>(requested), plan.entitySlotMutation)) {
        return false;
    }
    plan.sessionId = request.accountHandle;
    plan.delivery = Delivery::entitySlotNotification;
    plan.mutationDomain = MutationDomain::entitySlots;
    return true;
}

/** @return How many slots one authority mask names. */
[[nodiscard]] std::size_t
mask_slot_count(const service::entity_slots::EntitySlotMask& mask) noexcept {
    std::size_t slots = 0;
    for (const std::byte value : mask) {
        slots += static_cast<std::size_t>(std::popcount(std::to_integer<unsigned char>(value)));
    }
    return slots;
}

/**
 * Reports one msg 26 or msg 33. Neither returns a lease. Msg 21 does.
 * @param request Validated owned svc8 envelope.
 * @param expectReason True for msg 26, which trails a 3-bit reason after the mask.
 * @return True when the fixed body for that message type decodes.
 */
[[nodiscard]] bool report_authority_release(const service::Request& request,
                                            bool expectReason) noexcept {
    authority::Release decoded;
    const bool parsed = expectReason ? authority::parse_abandon(request.payload, decoded)
                                     : authority::parse_abdicate(request.payload, decoded);
    if (!parsed) {
        return false;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=authority result=noted type=%u "
                                      "selector=%u reason=%d slots=%zu",
                                      request.messageType,
                                      static_cast<unsigned>(decoded.selector),
                                      decoded.hasReason ? decoded.reason : 0,
                                      mask_slot_count(decoded.mask));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return true;
}

/**
 * Reports one msg 29, 31 or 32 answer. This host sends no msg 28 or msg 30, so an answer here is
 * the Client reconciling on its own. Nothing is staged.
 * @param request Validated owned svc8 envelope.
 * @return True when the body for that message type decodes.
 */
[[nodiscard]] bool report_query_answer(const service::Request& request) noexcept {
    namespace authority = service::entity_authority;
    authority::QueryAnswer answer;
    if (!authority::parse_query_answer(request.messageType, request.payload, answer)) {
        return false;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=authority result=ok type=%u corr=0x%08X "
                                      "selector=%d mask=%u",
                                      request.messageType,
                                      answer.correlation,
                                      answer.hasSelector ? static_cast<int>(answer.selector) : -1,
                                      static_cast<unsigned>(answer.hasMask));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return true;
}

/**
 * Reports one msg 27 purge request. The host does not answer it: the reply is msg 25, whose
 * consumer asserts unless the epoch is one above the Client's own, and nothing here tracks that.
 * @param request Validated owned svc8 envelope.
 * @return True when the fixed body is present.
 */
[[nodiscard]] bool report_request_purge(const service::Request& request) noexcept {
    std::int32_t reason = 0;
    if (!service::entity_authority::parse_request_purge(request.payload, reason)) {
        return false;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=activity stage=purge result=noted reason=%d", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return true;
}

/**
 * Prepares only the slots that are both held and in the returned mask.
 * @param request Validated owned svc8 envelope.
 * @param plan Cleared, then receives the chosen release mask.
 * @return True when the exact mask decodes and its session can stage a release.
 */
[[nodiscard]] bool prepare_release(const service::Request& request, ActivityPlan& plan) noexcept {
    service::entity_slots::EntitySlotMask decoded{};
    if (!service::entity_slots::decode_entity_slots(request.payload, decoded)) {
        return false;
    }
    state::activity::entity_slots::LeaseMask returned{};
    std::copy(decoded.begin(), decoded.end(), returned.begin());
    if (!state::activity::entity_slots::prepare_release(
            request.accountHandle, returned, plan.entitySlotMutation)) {
        return false;
    }
    plan.sessionId = request.accountHandle;
    plan.delivery = Delivery::none;
    plan.mutationDomain = MutationDomain::entitySlots;
    return true;
}

} // namespace

/** Routes one svc8 activity message and prepares any supported push transaction. */
bool process(std::uint64_t boundSessionId,
             std::span<const std::byte> requestBody,
             ActivityPlan& plan,
             bool& hasTransaction) noexcept {
    plan = {};
    hasTransaction = false;

    service::Request request;
    if (!service::parse_request(requestBody, request)) {
        report_message(0, 0, "parse");
        return false;
    }
    // Dispatch is on message type alone, and every handler keys off the envelope's own handle, so
    // nothing has to be bound first. The join request carries the session in the first place, and
    // it arrives on a link that has allocated nothing.
    bool prepared = false;
    if (request.messageType == epoch_message::kMessageType) {
        // Type 52 alone carries a zero handle, so its session is the one this link allocated.
        prepared = patch_epoch::prepare(boundSessionId, request, plan);
    } else if (request.messageType == high_water::kMessageType
               || request.messageType == client_keepalive::kMessageType) {
        // Both are one-way notices with nothing to answer.
        return true;
    } else if (request.messageType == kJoinRequestMessageType) {
        prepared = prepare_join(request, plan);
    } else if (request.messageType == service::entity_slot_request::kMessageType) {
        prepared = prepare_grant(request, plan);
    } else if (request.messageType == service::entity_slots::kRequestMessageType) {
        prepared = prepare_release(request, plan);
    } else if (request.messageType == service::state_refresh::kMessageType) {
        prepared = membership::prepare_refresh(request, plan);
    } else if (request.messageType == service::client_identity::kMessageType) {
        prepared = membership::prepare_identity(request, plan);
    } else if (request.messageType == service::client_authoritative_data::kMessageType) {
        prepared = membership::prepare_authoritative(request, plan);
    } else if (request.messageType == service::membership_acknowledgement::kMessageType) {
        prepared = membership::prepare_acknowledgement(request, plan);
    } else if (request.messageType == authority::kAbandonMessageType) {
        if (!report_authority_release(request, true)) {
            report_message(request.messageType, request.accountHandle, "parse");
        }
        return true;
    } else if (request.messageType == authority::kAbdicateMessageType) {
        if (!report_authority_release(request, false)) {
            report_message(request.messageType, request.accountHandle, "parse");
        }
        return true;
    } else if (request.messageType == service::incident::kMessageType) {
        report_incident(request);
        return true;
    } else if (request.messageType == authority::kRequestPurgeMessageType) {
        if (!report_request_purge(request)) {
            report_message(request.messageType, request.accountHandle, "parse");
        }
        return true;
    } else if (request.messageType == authority::kResetAcknowledgementMessageType
               || request.messageType == authority::kQueryPerBubbleMessageType
               || request.messageType == authority::kQueryResponseMessageType) {
        if (!report_query_answer(request)) {
            report_message(request.messageType, request.accountHandle, "parse");
        }
        return true;
    } else if (const char* name = accepted_name(request.messageType); name != nullptr) {
        // One-way with nothing to change here. Accepting is the whole contract.
        report_accepted(request.messageType, name, request.payload.size());
        return true;
    } else {
        // Later message handlers are independent. An owned envelope is a safe no-op.
        report_message(request.messageType, request.accountHandle, "unhandled");
        return true;
    }
    // A message that cannot be staged is reported and dropped. Failing the frame would leave the
    // Client's pending ring jammed.
    if (!prepared) {
        report_message(request.messageType, request.accountHandle, "prepare");
        plan = {};
        return true;
    }
    hasTransaction = true;
    return true;
}

} // namespace sunrise::server::bap::encrypted::activity_message
