#include "activity_message_route.h"

#include <algorithm>
#include <array>
#include <cstdio>

#include "../../../../core/logging/log.h"
#include "../../../../core/settings/settings.h"
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
#include "../../../../middleware/bap/activity_message/peer_ledger.h"
#include "../../../../middleware/bap/activity_message/sense_update.h"
#include "../../../../middleware/bap/activity_message/start_activity.h"
#include "../../../../middleware/bap/activity_message/telemetry.h"
#include "../../../../middleware/encoding/byte_order.h"
#include "../../../../state/activity/receipts/activity_receipts.h"
#include "../../../../state/activity/runtime.h"
#include "membership/activity_membership_route.h"
#include "middleware/bap/activity_message/activity_entity_slot_request_parser.h"
#include "patch_epoch/activity_patch_epoch_route.h"
#include "receipts/activity_message_receipts.h"

namespace sunrise::server::bap::encrypted::activity_message {
namespace {

namespace service = middleware::bap::activity_message;
namespace authority = service::entity_authority;
namespace client_keepalive = service::client_keepalive;
namespace high_water = service::high_water;
namespace epoch_message = service::patch_epoch;
namespace ledger = service::peer_ledger;
namespace telemetry = service::telemetry;
namespace store = state::activity::receipts;

/** Activity message type 3 starts the client join transaction. */
constexpr std::uint32_t kJoinRequestMessageType = 3;
/**
 * Activity message type 8 is a client-local request the transport converts into its own service.
 * On this route it is an authenticated but invalid use, never a second session allocation.
 */
constexpr std::uint32_t kLocalActivityHostMessageType = 8;

/**
 * Reports one inbound activity message, whatever the route goes on to do with it.
 * Without this line a type the client never sends reads the same as one handled in silence.
 * Nothing else says whether the client ever asks for or returns an entity slot.
 * @param request Parsed envelope.
 */
void report_arrival(const service::Request& request) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=inbound type=%u handle=0x%llX bytes=%zu",
                                      request.messageType,
                                      static_cast<unsigned long long>(request.accountHandle),
                                      request.payload.size());
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Reports one activity message the route did not stage, naming its type.
 * Every inbound activity message is one-way, so nothing here can jam the client's reply ring. An
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
 * Records one arrival against the message type's receipt row.
 * Every routed message calls this, so a type that arrives and changes nothing is still counted.
 * @param request Validated envelope.
 * @param verdict How completely the body was framed.
 * @param consumedBits Bits the parser used, or zero when the type has no parser.
 */
void record(const service::Request& request,
            store::Verdict verdict,
            std::size_t consumedBits) noexcept {
    if (!core::settings::get().server.activation.activityCompatibilityMirror) {
        // Off, the framing still runs and still reports; only the retained arrival is skipped.
        return;
    }
    store::Arrival arrival{};
    arrival.sessionId = request.accountHandle;
    arrival.messageType = request.messageType;
    arrival.payloadBytes = static_cast<std::uint32_t>(request.payload.size());
    arrival.peerHeardMask = request.peerHeardMask;
    arrival.consumedBits = static_cast<std::uint32_t>(consumedBits);
    arrival.verdict = verdict;
    static_cast<void>(store::record(arrival));
}

/** Tests whether a retained link binding still names its exact State and host generations. */
[[nodiscard]] bool binding_is_current(const ActivityClientBinding& binding) noexcept {
    if (binding.role == ActivityClientRole::privateCurrent) {
        return binding.session.sessionId != state::activity::kAbsentSessionId
               && binding.source.sessionId == binding.session.sessionId
               && binding.source.createdRevision == binding.session.createdRevision
               && state::activity::binding_matches(binding.session);
    }
    if (binding.role != ActivityClientRole::publicTarget
        || !state::activity::binding_matches(binding.session)
        || !state::activity::binding_matches(binding.source)) {
        return false;
    }
    server::gameplay::group::HostSessionBinding host{};
    return server::gameplay::group::host_session_for_activity(binding.session.sessionId, host)
           && host.generation == binding.hostGeneration
           && host.groupSessionId == binding.groupSessionId
           && host.source.sessionId == binding.source.sessionId
           && host.source.createdRevision == binding.source.createdRevision
           && host.target.sessionId == binding.session.sessionId
           && host.target.createdRevision == binding.session.createdRevision;
}

/**
 * Tests whether one message may mutate the State this link owns.
 * A mutating body names its session through the envelope handle. Join and patch-epoch messages
 * have separate ownership rules and do not use this helper.
 * @param binding Exact ActivityClient generation owned by this link.
 * @param request Validated envelope.
 * @return True when the envelope handle may drive a State mutation.
 */
[[nodiscard]] bool owns_session(const ActivityClientBinding& binding,
                                const service::Request& request) noexcept {
    return binding_is_current(binding) && request.accountHandle == binding.session.sessionId;
}

/**
 * Prepares the joined State and the whole initial lease mask as one mutation.
 * @param binding Exact ActivityClient generation already owned by this link.
 * @param request Validated owned svc8 envelope.
 * @param plan Cleared, then receives join scalars and the chosen lease mask.
 * @return True when the fixed join payload and current State can stage together.
 */
[[nodiscard]] bool prepare_join(const ActivityClientBinding& binding,
                                const service::Request& request,
                                ActivityPlan& plan) noexcept {
    service::JoinRequest parsed;
    if (!service::join_request::parse_join_request(request.payload, parsed)
        || parsed.sessionId != request.accountHandle) {
        return false;
    }
    if (binding_is_current(binding) && parsed.sessionId == binding.session.sessionId) {
        plan.bindingIntent = BindingIntent::preserveCurrent;
        plan.targetBinding = binding.session;
    } else if (server::gameplay::group::host_session_for_activity(parsed.sessionId, plan.publicHost)
               && state::activity::binding_matches(plan.publicHost.target)) {
        plan.bindingIntent = BindingIntent::publicTarget;
        plan.targetBinding = plan.publicHost.target;
    } else {
        return false;
    }
    // The client takes the low slots and the server keeps the reserve above them.
    const core::settings::server::gameplay::Settings& gameplay =
        core::settings::get().server.gameplay;
    const std::size_t reserve = core::settings::server::gameplay::effective_reserve(gameplay);
    const std::size_t granted = core::settings::server::gameplay::join_grant(gameplay);
    if (!state::activity::entity_slots::prepare_join(
            parsed.sessionId, parsed.memberKey, granted, reserve, plan.entitySlotMutation)) {
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

/** One framing-only message type and the handler that reads its body. */
struct FramingRoute {
    std::uint32_t type;
    receipts::Framed (*frame)(const service::Request&) noexcept;
};

/** Frames one abandon, which trails a reason after the mask. */
[[nodiscard]] receipts::Framed frame_abandon(const service::Request& request) noexcept {
    return receipts::frame_authority_release(request, true);
}

/** Frames one abdicate, which carries no reason. */
[[nodiscard]] receipts::Framed frame_abdicate(const service::Request& request) noexcept {
    return receipts::frame_authority_release(request, false);
}

/** Every message type this route frames and records without changing State. */
constexpr std::array<FramingRoute, 22> kFramingRoutes{{
    {service::sense_update::kMessageType, receipts::frame_sense_update},
    {kLocalActivityHostMessageType, receipts::frame_route_misuse},
    {telemetry::kReservationRequestType, receipts::frame_reservation_request},
    {ledger::kReleaseReservationType, receipts::frame_reservation_release},
    {ledger::kPeerLeaveType, receipts::frame_peer_leave},
    {client_keepalive::kMessageType, receipts::frame_client_keepalive},
    {service::incident::kMessageType, receipts::frame_incident},
    {authority::kAbandonMessageType, frame_abandon},
    {authority::kAbdicateMessageType, frame_abdicate},
    {authority::kRequestPurgeMessageType, receipts::frame_request_purge},
    {authority::kResetAcknowledgementMessageType, receipts::frame_query_answer},
    {authority::kQueryPerBubbleMessageType, receipts::frame_query_answer},
    {authority::kQueryResponseMessageType, receipts::frame_query_answer},
    {telemetry::kDebugCommandType, receipts::frame_debug_command},
    {ledger::kConnectivityFailureType, receipts::frame_connectivity_failure},
    {telemetry::kHeartbeatType, receipts::frame_heartbeat},
    {telemetry::kBugClawType, receipts::frame_opaque_scalar},
    {telemetry::kRefreshInspirationsType, receipts::frame_opaque_scalar},
    {telemetry::kLagSwitchType, receipts::frame_lag_switch},
    {telemetry::kConnectionQualityType, receipts::frame_connection_quality},
    {ledger::kSpeculativeMigrationType, receipts::frame_migration},
    {high_water::kMessageType, receipts::frame_high_water},
}};

/**
 * Frames one message that changes no State and records its receipt.
 * @param request Validated envelope.
 * @return Always true: a framing-only message can never fail the transport frame.
 */
[[nodiscard]] bool frame_only(const service::Request& request) noexcept {
    const auto row = std::find_if(kFramingRoutes.begin(),
                                  kFramingRoutes.end(),
                                  [&request](const FramingRoute& candidate) noexcept {
                                      return candidate.type == request.messageType;
                                  });
    const receipts::Framed framed =
        row != kFramingRoutes.end() ? row->frame(request) : receipts::frame_unknown(request);
    record(request, framed.verdict, framed.consumedBits);
    return true;
}

} // namespace

/** Routes one svc8 activity message and prepares any supported push transaction. */
bool process(const ActivityClientBinding& binding,
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
    report_arrival(request);
    // Join acquires or preserves an exact binding. Every other message, including type 52 and the
    // receipt-only types, must name the exact session already owned by this link before it can
    // mutate State or the receipt registry.
    const std::uint32_t messageType = request.messageType;
    const bool ownsMessage =
        messageType == kJoinRequestMessageType || owns_session(binding, request);
    if (!ownsMessage) {
        report_message(request.messageType, request.accountHandle, "unowned");
        record(request, store::Verdict::unowned, 0);
        return true;
    }
    bool prepared = false;
    if (request.messageType == epoch_message::kMessageType) {
        prepared = patch_epoch::prepare(request.accountHandle, request, plan);
    } else if (request.messageType == kJoinRequestMessageType) {
        prepared = prepare_join(binding, request, plan);
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
    } else if (request.messageType == service::start_activity::kMessageType) {
        // Off, the request is framed and recorded but no transition policy runs on it.
        if (!core::settings::get().server.activation.defaultClientActivation) {
            const receipts::Framed framed = receipts::frame_start_activity(request);
            record(request, framed.verdict, framed.consumedBits);
            return true;
        }
        prepared = membership::prepare_start_activity(request, plan);
    } else {
        return frame_only(request);
    }
    // A message that cannot be staged is reported and dropped. Failing the frame would leave the
    // client's pending ring jammed.
    if (!prepared) {
        report_message(request.messageType, request.accountHandle, "prepare");
        record(request, store::Verdict::malformed, 0);
        plan = {};
        return true;
    }
    record(request,
           store::Verdict::framed,
           request.payload.size() * middleware::encoding::kBitsPerByte);
    hasTransaction = true;
    return true;
}

} // namespace sunrise::server::bap::encrypted::activity_message
