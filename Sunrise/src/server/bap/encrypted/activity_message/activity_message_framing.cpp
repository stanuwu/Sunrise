#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>

#include "../../../../middleware/bap/activity_message/cinematic_incident.h"
#include "../../../../middleware/bap/activity_message/entity_authority.h"
#include "../../../../middleware/bap/activity_message/incident.h"
#include "../../../../middleware/bap/activity_message/player_trigger_incident.h"
#include "../../../../middleware/bap/activity_message/sense_update.h"
#include "../../../../state/activity_sdk/runtime.h"
#include "activity_message_route_internal.h"

namespace sunrise::server::bap::encrypted::activity_message {
namespace {

namespace sense_update = middleware::bap::activity_message::sense_update;
namespace cinematic_incident = middleware::bap::activity_message::cinematic_incident;
namespace player_trigger_incident = middleware::bap::activity_message::player_trigger_incident;
namespace activity_sdk = state::activity_sdk;

/** Borrowed exact connection and SDK state used only during one msg-6 route call. */
struct SenseResolverContext final {
    const ActivityClientBinding* binding{};
    const RosterDecodeMap* roster{};
    const activity_sdk::Catalog* catalog{};
};

/** Resolves the group one sense registry key names, against this connection's roster. */
[[nodiscard]] sense_update::TargetStatus resolve_sense_group(
    const void* raw, std::uint32_t registryKey, sense_update::GroupTarget& output) noexcept {
    output = {};
    const auto* const context = static_cast<const SenseResolverContext*>(raw);
    if (context == nullptr || context->binding == nullptr || context->roster == nullptr
        || context->catalog == nullptr) {
        return sense_update::TargetStatus::targetUnavailable;
    }
    const RosterDecodeEntry* const entry = find_roster_decode_entry(
        *context->roster, context->binding->bindingGeneration, registryKey);
    if (entry == nullptr || entry->objectTag == 0) {
        return sense_update::TargetStatus::targetUnavailable;
    }
    const auto objects = context->catalog->objects();
    const activity_sdk::format::Object* match = nullptr;
    for (const activity_sdk::format::Object& object : objects) {
        if (object.objectTag != entry->objectTag) {
            continue;
        }
        if (match != nullptr) {
            return sense_update::TargetStatus::targetUnavailable;
        }
        match = &object;
    }
    if (match == nullptr || match->objectKey != registryKey) {
        return sense_update::TargetStatus::targetUnavailable;
    }
    output.objectTag = entry->objectTag;
    output.objectRow = static_cast<std::uint32_t>(match - objects.data());
    return sense_update::TargetStatus::resolved;
}

/** A Sense slot must resolve within the exact published object group. */
[[nodiscard]] sense_update::TargetStatus
resolve_sense_slot(const void* raw,
                   const sense_update::GroupTarget& group,
                   std::uint8_t slotType,
                   std::uint16_t slotIndex,
                   sense_update::SlotTarget& output) noexcept {
    output = {};
    const auto* const context = static_cast<const SenseResolverContext*>(raw);
    if (context == nullptr || context->catalog == nullptr
        || group.objectRow >= context->catalog->objects().size()) {
        return sense_update::TargetStatus::targetUnavailable;
    }
    const activity_sdk::format::Object& object = context->catalog->objects()[group.objectRow];
    if (object.objectTag != group.objectTag) {
        return sense_update::TargetStatus::targetUnavailable;
    }
    const auto allSlots = context->catalog->slots();
    const activity_sdk::format::Slot* match = nullptr;
    for (const activity_sdk::format::Slot& slot :
         activity_sdk::object_slots(*context->catalog, object)) {
        if (slot.slotType != slotType || slot.slotIndex != slotIndex) {
            continue;
        }
        if (match != nullptr) {
            return sense_update::TargetStatus::targetUnavailable;
        }
        match = &slot;
    }
    if (match == nullptr || match->objectIndex != group.objectRow) {
        return sense_update::TargetStatus::targetUnavailable;
    }
    output.slotRow = static_cast<std::uint32_t>(match - allSlots.data());
    output.senseSchema = match->senseSchema;
    if (match->componentClass == activity_sdk::format::kAbsentIndex || match->senseSchema == 0
        || match->senseSchema == activity_sdk::format::kAbsentIndex
        || (match->flags & activity_sdk::format::kSlotSchemaJoinExact) == 0) {
        return sense_update::TargetStatus::schemaUnavailable;
    }
    // Native Sense codecs dispatch by the authored schema handle. No SDK reflection row exists.
    output.schemaRow = match->senseSchema;
    return sense_update::TargetStatus::resolved;
}

/** One framing-only adapter and the handler that reads its body. */
struct FramingRoute {
    IngressAdapter adapter;
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

/** Every adapter this route frames and records without changing State. */
constexpr std::array<FramingRoute, 19> kFramingRoutes{{
    {IngressAdapter::routeMisuseReceipt, receipts::frame_route_misuse},
    {IngressAdapter::reservationRequest, receipts::frame_reservation_request},
    {IngressAdapter::reservationRelease, receipts::frame_reservation_release},
    {IngressAdapter::peerLeave, receipts::frame_peer_leave},
    {IngressAdapter::clientKeepalive, receipts::frame_client_keepalive},
    {IngressAdapter::incidentHostIncident, receipts::frame_incident},
    {IngressAdapter::authorityAbandon, frame_abandon},
    {IngressAdapter::authorityAbdicate, frame_abdicate},
    {IngressAdapter::authorityRequestPurge, receipts::frame_request_purge},
    {IngressAdapter::authorityResetAcknowledgement, receipts::frame_query_answer},
    {IngressAdapter::authorityQueryAnswer, receipts::frame_query_answer},
    {IngressAdapter::debugCommandQuarantine, receipts::frame_debug_command},
    {IngressAdapter::connectivityFailure, receipts::frame_connectivity_failure},
    {IngressAdapter::heartbeat, receipts::frame_heartbeat},
    {IngressAdapter::opaqueScalar, receipts::frame_opaque_scalar},
    {IngressAdapter::lagSwitch, receipts::frame_lag_switch},
    {IngressAdapter::connectionQuality, receipts::frame_connection_quality},
    {IngressAdapter::migration, receipts::frame_migration},
    {IngressAdapter::highWater, receipts::frame_high_water},
}};

} // namespace

/**
 * Frames one message and records its receipt.
 * @param binding Exact ActivityClient generation owned by this link.
 * @param rosterDecode Last complete msg-5 identity map delivered on this same link.
 * @param adapter Ingress adapter the communication route named for this message type.
 * @param request Validated envelope.
 * @return Always true: a framing-only message can never fail the transport frame.
 */
bool frame_only(const ActivityClientBinding& binding,
                const RosterDecodeMap& rosterDecode,
                IngressAdapter adapter,
                const service::Request& request) noexcept {
    const auto row = std::find_if(
        kFramingRoutes.begin(),
        kFramingRoutes.end(),
        [adapter](const FramingRoute& candidate) noexcept { return candidate.adapter == adapter; });
    service::incident::Incident parsedIncident{};
    const bool isIncident = adapter == IngressAdapter::incidentHostIncident;
    const bool isSense = adapter == IngressAdapter::senseUpdateHostSense;
    // Packet storage is shared by diagnostics and mission delivery; keep its bounded
    // arrays off the networking thread's stack, including the submission copy.
    std::unique_ptr<sense_update::SenseUpdate> parsedSense;
    std::unique_ptr<server::activity::host::SenseInput> senseInput;
    if (isSense) {
        parsedSense.reset(new (std::nothrow) sense_update::SenseUpdate{});
        senseInput.reset(new (std::nothrow) server::activity::host::SenseInput{});
        if (!parsedSense || !senseInput) {
            report_message(request.messageType, request.sessionId, "sense_storage_unavailable");
            return false;
        }
    }
    receipts::Framed framed{};
    if (isSense) {
        const activity_sdk::Snapshot catalog = activity_sdk::snapshot();
        const SenseResolverContext context{&binding, &rosterDecode, catalog.get()};
        const sense_update::Resolver resolver{&context, resolve_sense_group, resolve_sense_slot};
        std::size_t consumedBits = 0;
        static_cast<void>(sense_update::decode_sense_update(
            request.payload, resolver, *parsedSense, consumedBits));
        framed = receipts::frame_sense_update(request, *parsedSense);
    } else {
        framed = isIncident ? receipts::frame_incident_copy(request, parsedIncident)
                 : row != kFramingRoutes.end() ? row->frame(request)
                                               : receipts::frame_unknown(request);
    }
    DiagnosticBody diagnostic{};
    diagnostic.consumedBits = framed.consumedBits;
    diagnostic.status = diagnostic_status(framed, isIncident);
    diagnostic.sense = isSense ? &parsedSense->decoded : nullptr;
    if (isSense && parsedSense->decoded.status == sense_update::DecodeStatus::partial) {
        diagnostic.status = server::activity::host::ClientMessageStatus::decodedPartial;
    }
    const std::uint64_t clientMessageSequence = record(request,
                                                       framed.verdict,
                                                       framed.consumedBits,
                                                       &binding.session,
                                                       binding.bindingGeneration,
                                                       diagnostic);
    if (isSense && parsedSense->decoded.status != sense_update::DecodeStatus::malformed) {
        server::activity::host::SenseInput& input = *senseInput;
        input.binding = binding.session;
        input.sourceGeneration = binding.bindingGeneration;
        input.clientMessageSequence = clientMessageSequence;
        input.epochFirst = parsedSense->epoch.first;
        input.epochSecond = parsedSense->epoch.second;
        input.payloadBytes = static_cast<std::uint32_t>(request.payload.size());
        input.peerHeardMask = request.peerHeardMask;
        input.tailBits = parsedSense->tailBits;
        input.consumedBits = static_cast<std::uint32_t>(framed.consumedBits);
        input.firstGroupBits = parsedSense->firstGroupBits;
        input.firstRegistryKey = parsedSense->firstRegistryKey;
        input.groupsSeen = parsedSense->decoded.groupsSeen;
        input.groupsDecoded = parsedSense->decoded.groupsDecoded;
        input.groupsSkipped = parsedSense->decoded.groupsSkipped;
        input.objectsSeen = parsedSense->decoded.objectsSeen;
        input.objectsDecoded = parsedSense->decoded.objectsDecoded;
        input.firstSlotIndex = parsedSense->firstSlotIndex;
        input.firstSlotType = parsedSense->firstSlotType;
        input.decodeStatus = parsedSense->decoded.status;
        input.verdict = framed.verdict;
        input.decoded = parsedSense->decoded;
        input.hasFirstObject = parsedSense->hasFirstObject;
        const auto refusal = server::activity::host::submit_sense(input);
        if (refusal != server::activity::host::IngressRefusal::none) {
            report_message(request.messageType,
                           request.sessionId,
                           server::activity::host::ingress_refusal_name(refusal));
        }
    } else if (isIncident && framed.verdict == store::Verdict::framed) {
        server::activity::host::IncidentInput input{};
        input.binding = binding.session;
        input.incident = parsedIncident;
        input.sourceGeneration = binding.bindingGeneration;
        input.clientMessageSequence = clientMessageSequence;
        input.payloadBytes = static_cast<std::uint32_t>(request.payload.size());
        if (parsedIncident.primaryTarget == player_trigger_incident::kPrimaryTarget
            && parsedIncident.payloadLength == player_trigger_incident::kPayloadBytes) {
            input.hasPlayerTrigger = player_trigger_incident::decode(
                std::span(parsedIncident.payload).first(parsedIncident.payloadLength),
                input.playerTrigger);
        }
        if (parsedIncident.payloadLength == cinematic_incident::kPayloadBytes
            && cinematic_incident::signal_for_target(parsedIncident.primaryTarget,
                                                     input.cinematicSignal)) {
            input.hasCinematic = cinematic_incident::decode(
                std::span(parsedIncident.payload).first(parsedIncident.payloadLength),
                input.cinematic);
        }
        const auto refusal = server::activity::host::submit_incident(input);
        if (refusal != server::activity::host::IngressRefusal::none) {
            report_message(request.messageType,
                           request.sessionId,
                           server::activity::host::ingress_refusal_name(refusal));
        }
    }
    return true;
}

/** Retains one exact msg-31 or msg-32 answer until its authenticated frame commits. */
bool prepare_authority_query_answer(const ActivityClientBinding& binding,
                                    const RosterDecodeMap& rosterDecode,
                                    IngressAdapter adapter,
                                    const service::Request& request,
                                    ActivityPlan& plan,
                                    bool& hasTransaction) noexcept {
    service::entity_authority::QueryAnswer answer{};
    const bool parsed =
        service::entity_authority::parse_query_answer(request.messageType, request.payload, answer);
    if (!frame_only(binding, rosterDecode, adapter, request)) {
        return false;
    }
    if (!parsed
        || (request.messageType != service::entity_authority::kQueryPerBubbleMessageType
            && request.messageType != service::entity_authority::kQueryResponseMessageType)) {
        return true;
    }
    plan.sessionId = request.sessionId;
    plan.authorityQuery.answer = answer;
    plan.authorityQuery.sourceGeneration = binding.bindingGeneration;
    plan.authorityQuery.pending = true;
    plan.delivery = Delivery::none;
    plan.mutationDomain = MutationDomain::authorityQuery;
    hasTransaction = true;
    return true;
}

/** The client reported it is leaving, so the roster it holds is owed its leave delta. */
bool prepare_peer_leave(const ActivityClientBinding& binding,
                        const RosterDecodeMap& rosterDecode,
                        IngressAdapter adapter,
                        const service::Request& request,
                        ActivityPlan& plan,
                        bool& hasTransaction) noexcept {
    if (!frame_only(binding, rosterDecode, adapter, request)) {
        return false;
    }
    // With no delivered roster the client registered nothing on this link, so nothing is owed.
    if (!rosterDecode.valid || rosterDecode.bindingGeneration != binding.bindingGeneration) {
        return true;
    }
    plan.sessionId = request.sessionId;
    plan.delivery = Delivery::leaveNotification;
    plan.mutationDomain = MutationDomain::none;
    hasTransaction = true;
    return true;
}

/** A purge answer preserves the requesting client's complete mask and reason. */
bool prepare_authority_purge(const ActivityClientBinding& binding,
                             const RosterDecodeMap& rosterDecode,
                             IngressAdapter adapter,
                             const service::Request& request,
                             ActivityPlan& plan,
                             bool& hasTransaction) noexcept {
    service::entity_authority::PurgeRequest purge{};
    const bool parsed = service::entity_authority::parse_request_purge(request.payload, purge);
    if (!frame_only(binding, rosterDecode, adapter, request)) {
        return false;
    }
    if (!parsed) {
        return true;
    }
    plan.sessionId = request.sessionId;
    plan.authorityPurge.body.slots = purge.mask;
    plan.authorityPurge.body.reason = static_cast<std::int8_t>(purge.reason);
    plan.authorityPurge.body.epoch = static_cast<std::uint8_t>(binding.replicationEpoch + 1U);
    plan.authorityPurge.sourceGeneration = binding.bindingGeneration;
    plan.authorityPurge.pending = true;
    plan.delivery = Delivery::purgeNotification;
    plan.mutationDomain = MutationDomain::authorityPurge;
    hasTransaction = true;
    return true;
}

/** A valid abdication changes ownership only after its frame commits. */
bool prepare_authority_abdication(const ActivityClientBinding& binding,
                                  const RosterDecodeMap& rosterDecode,
                                  IngressAdapter adapter,
                                  const service::Request& request,
                                  ActivityPlan& plan,
                                  bool& hasTransaction) noexcept {
    service::entity_authority::Release release{};
    const bool parsed = service::entity_authority::parse_abdicate(request.payload, release);
    if (!frame_only(binding, rosterDecode, adapter, request)) {
        return false;
    }
    if (!parsed) {
        return true;
    }
    plan.sessionId = request.sessionId;
    plan.authorityAbdication.sourceGeneration = binding.bindingGeneration;
    plan.authorityAbdication.entities = release.mask;
    plan.authorityAbdication.bubble = release.selector;
    plan.authorityAbdication.pending = true;
    plan.delivery = Delivery::none;
    plan.mutationDomain = MutationDomain::authorityAbdication;
    hasTransaction = true;
    return true;
}

/** Retains one exact msg-29 acknowledgement until its authenticated frame commits. */
bool prepare_authority_reset_acknowledgement(const ActivityClientBinding& binding,
                                             const RosterDecodeMap& rosterDecode,
                                             IngressAdapter adapter,
                                             const service::Request& request,
                                             ActivityPlan& plan,
                                             bool& hasTransaction) noexcept {
    service::entity_authority::QueryAnswer answer{};
    const bool parsed =
        service::entity_authority::parse_query_answer(request.messageType, request.payload, answer);
    if (!frame_only(binding, rosterDecode, adapter, request)) {
        return false;
    }
    if (!parsed
        || request.messageType != service::entity_authority::kResetAcknowledgementMessageType) {
        return true;
    }
    plan.sessionId = request.sessionId;
    plan.authorityReset.answer = answer;
    plan.authorityReset.sourceGeneration = binding.bindingGeneration;
    plan.authorityReset.pending = true;
    plan.delivery = Delivery::none;
    plan.mutationDomain = MutationDomain::authorityReset;
    hasTransaction = true;
    return true;
}

} // namespace sunrise::server::bap::encrypted::activity_message
