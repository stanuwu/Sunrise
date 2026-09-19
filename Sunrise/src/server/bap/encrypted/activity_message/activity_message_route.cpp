#include "activity_message_route.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#include "../../../../core/logging/log.h"
#include "../../../../core/settings/settings.h"
#include "../../../../middleware/bap/activity_message/activity_join_request_parser.h"
#include "../../../../middleware/bap/activity_message/activity_message_request_parser.h"
#include "../../../../middleware/bap/activity_message/entity_slots.h"
#include "../../../../middleware/bap/activity_message/peer_ledger.h"
#include "../../../../middleware/bap/activity_message/wire_schema/activity_communication_route.h"
#include "../../../../middleware/crypto/hmac.h"
#include "../../../../middleware/crypto/random_bytes.h"
#include "../../../../middleware/encoding/byte_order.h"
#include "../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../state/activity/receipts/activity_receipts.h"
#include "../../../../state/activity/runtime.h"
#include "../../../../state/activity/shared_target.h"
#include "../../../../state/activity_sdk/runtime.h"
#include "../../../activity/host_runtime.h"
#include "../../../gameplay/gameplay_advertisement.h"
#include "../push/activity/activity_arrival.h"
#include "../push/activity/internal.h"
#include "activity_message_route_internal.h"
#include "membership/activity_membership_route.h"
#include "membership/activity_reservations_route.h"
#include "middleware/bap/activity_message/activity_entity_slot_request_parser.h"
#include "native_member_identity.h"
#include "patch_epoch/activity_patch_epoch_route.h"
#include "receipts/activity_message_receipts.h"

namespace sunrise::server::bap::encrypted::activity_message {
namespace {

namespace activity_sdk = state::activity_sdk;

/** Reads the current membership without naming a bubble. */
constexpr std::uint32_t kCurrentRevision = 0;
constexpr std::int32_t kNoBubble = -1;

/** Process-private HMAC key width used only for run-local diagnostic correlation. */
constexpr std::size_t kFingerprintKeySize = 32;
/** Domain prefix keeps this diagnostic use separate from protocol authentication. */
constexpr std::array<std::byte, 4> kFingerprintDomain{
    std::byte{'A'}, std::byte{'H'}, std::byte{'I'}, std::byte{'1'}};

/** One process-private key, unavailable when system entropy failed. */
struct FingerprintKey final {
    std::array<std::byte, kFingerprintKeySize> bytes{};
    bool available{};
};

/** One non-reversible, run-local body correlation value. */
struct Fingerprint final {
    std::uint64_t value{};
    bool present{};
};

/** @return Process-private fingerprint key, initialized once from Windows system entropy. */
[[nodiscard]] const FingerprintKey& fingerprint_key() noexcept {
    static const FingerprintKey key = []() noexcept {
        FingerprintKey value{};
        value.available = middleware::crypto::random::fill(value.bytes);
        return value;
    }();
    return key;
}

/** @return Keyed, run-local correlation value without retaining the borrowed payload. */
[[nodiscard]] Fingerprint payload_fingerprint(std::uint32_t messageType,
                                              std::span<const std::byte> payload) noexcept {
    const FingerprintKey& key = fingerprint_key();
    if (!key.available) {
        return {};
    }
    std::array<std::byte, kFingerprintDomain.size() + middleware::encoding::kU32Size> domain{};
    std::copy(kFingerprintDomain.begin(), kFingerprintDomain.end(), domain.begin());
    middleware::encoding::write_u32_be(
        std::span(domain).subspan<kFingerprintDomain.size(), middleware::encoding::kU32Size>(),
        messageType);
    middleware::crypto::hmac::Digest digest{};
    if (!middleware::crypto::hmac::authenticate(
            middleware::crypto::hmac::Algorithm::sha256, key.bytes, domain, payload, digest)
        || digest.size < middleware::encoding::kU64Size) {
        return {};
    }
    Fingerprint result{};
    result.value = middleware::encoding::read_u64_be(
        std::span(digest.bytes).first<middleware::encoding::kU64Size>());
    result.present = true;
    return result;
}

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
                                      static_cast<unsigned long long>(request.sessionId),
                                      request.payload.size());
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Names a retract the host declined, because a member the client believes it dropped but the
 * host still carries reads exactly like a lost message otherwise.
 */
void report_release_refusal(const service::Request& request,
                            const membership::ReleaseRefusalReport& refusal) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=activity stage=reservation_release result=%s soid=0x%llX peer=0x%llX",
        refusal.refusal == state::activity::reservations::ReleaseRefusal::hostRow ? "host_row"
                                                                                  : "lease_held",
        static_cast<unsigned long long>(request.sessionId),
        static_cast<unsigned long long>(refusal.peerKey));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
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
    return binding_is_current(binding) && request.sessionId == binding.session.sessionId;
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
    const auto refuse = [](const char* reason) noexcept {
        core::log::write(core::log::Channel::server, core::log::Level::warn, reason);
        return false;
    };
    service::JoinRequest parsed;
    if (!service::join_request::parse_join_request(request.payload, parsed)
        || parsed.sessionId != request.sessionId) {
        return refuse("ev=activity stage=join result=refused reason=payload_identity");
    }
    const auto identity = native_member_identity(parsed.identity);
    const bool shared = core::settings::hosts_session();
    if (binding_is_current(binding) && parsed.sessionId == binding.session.sessionId) {
        plan.bindingIntent = BindingIntent::preserveCurrent;
        plan.targetBinding = binding.session;
        // The join burst carries the membership body, so its logical host must already be ready.
        // Private activities use one stable Bubble Host. Public regions use their citizen host.
        const std::int32_t arrival = push::activity::effective_region(binding.session).index;
        if (push::activity::private_region(binding.session, binding.bindingGeneration, arrival)) {
            if (!server::gameplay::complete_private_host_session(binding.session, arrival)) {
                return refuse("ev=activity stage=join result=refused reason=private_host");
            }
        } else {
            server::gameplay::complete_host_session(
                binding.session,
                arrival,
                push::activity::public_region(binding.session, binding.bindingGeneration, arrival));
        }
    } else if (server::gameplay::group::host_session_for_activity(parsed.sessionId, plan.publicHost)
               && state::activity::binding_matches(plan.publicHost.target)) {
        plan.bindingIntent = BindingIntent::publicTarget;
        plan.targetBinding = plan.publicHost.target;
    } else if (shared
               && state::activity::shared_target(parsed.sessionId, identity, plan.targetBinding)) {
        plan.bindingIntent = BindingIntent::sharedTarget;
    } else {
        return refuse("ev=activity stage=join result=refused reason=target_authorization");
    }
    // The client takes the low slots and the server keeps the reserve above them.
    const core::settings::server::gameplay::Settings& gameplay =
        core::settings::get().server.gameplay;
    const std::size_t reserve = core::settings::server::gameplay::effective_reserve(gameplay);
    const std::size_t granted = core::settings::server::gameplay::join_grant(gameplay);
    bool prepared = false;
    if (shared && plan.bindingIntent == BindingIntent::publicTarget) {
        std::array<state::activity::SessionBinding, state::activity::kSessionCapacity> sources{};
        std::size_t count{};
        server::gameplay::group::host_session_sources(plan.publicHost.generation, sources, count);
        // A pooled public host accepts only a player authorized by a live source that advertised
        // it. prepare_join and commit recheck the bound account, selected character and generation.
        for (std::size_t index = 0; index < count && !prepared; ++index) {
            prepared = state::activity::entity_slots::prepare_join(parsed.sessionId,
                                                                   parsed.memberKey,
                                                                   granted,
                                                                   reserve,
                                                                   plan.entitySlotMutation,
                                                                   &identity,
                                                                   &sources[index]);
        }
    } else {
        prepared = state::activity::entity_slots::prepare_join(parsed.sessionId,
                                                               parsed.memberKey,
                                                               granted,
                                                               reserve,
                                                               plan.entitySlotMutation,
                                                               shared ? &identity : nullptr,
                                                               nullptr);
    }
    if (!prepared) {
        return refuse("ev=activity stage=join result=refused reason=member_lease");
    }
    plan.correlation = parsed.correlation;
    plan.sessionId = parsed.sessionId;
    plan.joinCharacterSoid = parsed.characterSoid;
    plan.delivery = Delivery::joinNotifications;
    plan.mutationDomain = MutationDomain::entitySlots;
    // Keep the upstream public-membership switch effective for the co-op initial burst too.
    // Admission and native identity still commit; only the unsolicited membership is withheld.
    if (plan.bindingIntent == BindingIntent::publicTarget
        && !core::settings::get().server.activation.activityPublicMembership) {
        return true;
    }
    // Read, never committed: the domain above is what the commit acts on. Every shared join
    // takes its body from this join's own mutation, so the member the client recognises as the
    // local player is its own row in the session the envelope names. Copying the private
    // source's table would publish that table's revision under the target's name.
    if (shared) {
        if (!state::activity::membership::prepare_join_snapshot(plan.entitySlotMutation,
                                                                plan.membershipMutation)) {
            return refuse("ev=activity stage=join result=refused reason=membership_snapshot");
        }
    } else if (plan.bindingIntent == BindingIntent::publicTarget
               && state::activity::binding_matches(plan.publicHost.source)) {
        // Embedded solo retains the local member table; only shared targets own peers.
        static_cast<void>(
            state::activity::membership::prepare_refresh(plan.publicHost.source.sessionId,
                                                         kCurrentRevision,
                                                         kNoBubble,
                                                         plan.membershipMutation));
    } else if (plan.bindingIntent == BindingIntent::preserveCurrent) {
        // A private join's burst carries the seed membership; the commit lands the same seed.
        static_cast<void>(
            push::activity::prepare_join_seed_snapshot(plan.targetBinding.createdRevision,
                                                       parsed.memberKey,
                                                       parsed.characterSoid,
                                                       plan.membershipMutation));
    }
    return true;
}

/**
 * Prepares only currently free slots for one positive client request.
 * The grant is exactly what the client asked for. An ask above the slot count degrades to every
 * remaining free slot.
 * @param request Validated owned svc8 envelope.
 * @param plan Cleared, then receives the chosen lease mask.
 * @return True for a valid positive request, including an exhausted zero-mask grant.
 */
[[nodiscard]] bool prepare_grant(const service::Request& request, ActivityPlan& plan) noexcept {
    std::int32_t requested = 0;
    if (!service::entity_slot_request::parse_entity_slot_request(request.payload, requested)
        || requested <= 0
        || !state::activity::entity_slots::prepare_grant(
            request.sessionId, static_cast<std::size_t>(requested), plan.entitySlotMutation)) {
        return false;
    }
    plan.sessionId = request.sessionId;
    plan.entitySlotsRequested.requestedCount = requested;
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
            request.sessionId, returned, plan.entitySlotMutation)) {
        return false;
    }
    plan.returnedEntitySlots = decoded;
    plan.hasReturnedEntitySlots = true;
    plan.sessionId = request.sessionId;
    plan.delivery = Delivery::none;
    plan.mutationDomain = MutationDomain::entitySlots;
    return true;
}

} // namespace

/** Reports one activity message the route did not stage, naming its type. */
void report_message(std::uint32_t messageType,
                    std::uint64_t sessionId,
                    const char* reason) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=message result=skip type=%u "
                                      "handle=0x%llX reason=%s",
                                      messageType,
                                      static_cast<unsigned long long>(sessionId),
                                      reason);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Maps one framing-only parser result onto a diagnostic body status. */
server::activity::host::ClientMessageStatus diagnostic_status(const receipts::Framed& framed,
                                                              bool incident) noexcept {
    using Status = server::activity::host::ClientMessageStatus;
    switch (framed.verdict) {
    case store::Verdict::framed:
        return incident ? Status::outerDecoded : Status::decoded;
    case store::Verdict::partial:
        return framed.consumedBits == 0 ? Status::opaque : Status::prefixOnly;
    case store::Verdict::malformed:
        return Status::malformed;
    case store::Verdict::quarantined:
        return Status::quarantined;
    case store::Verdict::absent:
    case store::Verdict::unowned:
        return Status::unclassified;
    }
    return Status::unclassified;
}

/** Records one arrival in the aggregate receipts and the owned diagnostic history. */
std::uint64_t record(const service::Request& request,
                     store::Verdict receiptVerdict,
                     std::size_t receiptConsumedBits,
                     const state::activity::SessionBinding* ownedBinding,
                     std::uint64_t sourceGeneration,
                     const DiagnosticBody& diagnostic,
                     bool zeroHandleOwned) noexcept {
    std::uint64_t sequence = 0;
    const bool exactEnvelopeOwner =
        ownedBinding != nullptr && ownedBinding->sessionId == request.sessionId;
    const bool exactZeroHandleOwner =
        ownedBinding != nullptr && zeroHandleOwned && request.sessionId == 0;
    if (exactEnvelopeOwner || exactZeroHandleOwner) {
        const Fingerprint fingerprint = payload_fingerprint(request.messageType, request.payload);
        server::activity::host::ClientMessageInput input{};
        input.binding = *ownedBinding;
        input.sourceGeneration = sourceGeneration;
        input.payloadFingerprint = fingerprint.value;
        input.messageType = request.messageType;
        input.payloadBytes = static_cast<std::uint32_t>(request.payload.size());
        input.peerHeardMask = request.peerHeardMask;
        input.consumedBits = static_cast<std::uint32_t>(diagnostic.consumedBits);
        input.status = diagnostic.status;
        input.hasPayloadFingerprint = fingerprint.present;
        if (diagnostic.authoritative != nullptr) {
            input.authoritative = *diagnostic.authoritative;
            input.hasAuthoritative = true;
        }
        communication::ActivityCommunicationRoute route{};
        const bool executableRoute =
            activity_sdk::executable_communication_route(request.messageType, route);
        // Message handlers parse their own authored native body. Routing retains no parallel
        // schema-driven scalar decode.
        sequence = server::activity::host::record_client_message(input, diagnostic.sense);
        if (sequence != 0 && request.messageType != service::entity_slot_request::kMessageType
            && executableRoute
            && route.ingressDelivery == communication::IngressDeliveryPolicy::protocolHostInput
            && !server::activity::host::submit_client_message(input, sequence)) {
            report_message(request.messageType, request.sessionId, "mission_ingress_refused");
        }
    }
    store::Arrival arrival{};
    arrival.sessionId = request.sessionId;
    arrival.messageType = request.messageType;
    arrival.payloadBytes = static_cast<std::uint32_t>(request.payload.size());
    arrival.peerHeardMask = request.peerHeardMask;
    arrival.consumedBits = static_cast<std::uint32_t>(receiptConsumedBits);
    arrival.verdict = receiptVerdict;
    static_cast<void>(store::record(arrival));
    return sequence;
}

/** Routes one svc8 activity message and prepares any supported push transaction. */
bool process(const ActivityClientBinding& binding,
             const RosterDecodeMap& rosterDecode,
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
    communication::ActivityCommunicationRoute route{};
    const bool executableRoute =
        activity_sdk::executable_communication_route(request.messageType, route);
    const IngressAdapter adapter = executableRoute ? route.ingressAdapter : IngressAdapter::none;
    // Join acquires a binding. Msg52 may name the current binding or use its zero-handle form.
    const bool acquiresBinding = adapter == IngressAdapter::joinRequestStateJoin;
    const bool zeroHandlePatchEpoch = adapter == IngressAdapter::patchEpochStateEpoch;
    const bool currentOwnsEnvelope = owns_session(binding, request);
    const bool ownsMessage =
        acquiresBinding
        || (zeroHandlePatchEpoch
                ? currentOwnsEnvelope || (binding_is_current(binding) && request.sessionId == 0)
                : currentOwnsEnvelope);
    if (!ownsMessage) {
        report_message(request.messageType, request.sessionId, "unowned");
        static_cast<void>(record(request, store::Verdict::unowned, 0, nullptr, 0));
        return true;
    }
    bool prepared = false;
    switch (adapter) {
    case IngressAdapter::connectivityFailure: {
        service::peer_ledger::ConnectivityFailure failure{};
        std::size_t consumed = 0;
        plan.hasRelayConnectivityFailure =
            service::peer_ledger::parse_connectivity_failure(request.payload, failure, consumed);
        return frame_only(binding, rosterDecode, adapter, request);
    }
    case IngressAdapter::reservationRequest:
        prepared = membership::prepare_reservations(request, plan);
        break;
    case IngressAdapter::reservationRelease: {
        membership::ReleaseRefusalReport refusedRelease{};
        prepared = membership::prepare_reservation_release(request, plan, refusedRelease);
        // A retract the host declines to act on is a complete, well-formed message that changes
        // nothing. Framing it says so; the malformed prepare path below would not.
        if (!prepared
            && refusedRelease.refusal != state::activity::reservations::ReleaseRefusal::none) {
            plan = {};
            report_release_refusal(request, refusedRelease);
            return frame_only(binding, rosterDecode, adapter, request);
        }
        break;
    }
    case IngressAdapter::patchEpochStateEpoch:
        prepared = patch_epoch::prepare(binding.session.sessionId, request, plan);
        break;
    case IngressAdapter::joinRequestStateJoin:
        prepared = prepare_join(binding, request, plan);
        break;
    case IngressAdapter::entitySlotRequestStateSlots:
        prepared = prepare_grant(request, plan);
        break;
    case IngressAdapter::entitySlotsStateSlots:
        prepared = prepare_release(request, plan);
        break;
    case IngressAdapter::stateRefreshMembership:
        prepared = membership::prepare_refresh(request, plan);
        break;
    case IngressAdapter::clientIdentityMembership:
        prepared = membership::prepare_identity(request, plan);
        break;
    case IngressAdapter::clientAuthoritativeDataMembership:
        prepared = membership::prepare_authoritative(request, plan);
        if (prepared) {
            plan.transportBindingGeneration = binding.bindingGeneration;
        }
        break;
    case IngressAdapter::membershipAcknowledgement:
        prepared = membership::prepare_acknowledgement(request, plan);
        break;
    case IngressAdapter::startActivityOptionalStateRefresh:
        // Off, the request is framed and recorded but no transition policy runs on it.
        if (!core::settings::get().server.activation.defaultClientActivation) {
            const receipts::Framed framed = receipts::frame_start_activity(request);
            DiagnosticBody diagnostic{};
            diagnostic.consumedBits = framed.consumedBits;
            diagnostic.status = diagnostic_status(framed, false);
            static_cast<void>(record(request,
                                     framed.verdict,
                                     framed.consumedBits,
                                     &binding.session,
                                     binding.bindingGeneration,
                                     diagnostic));
            return true;
        }
        prepared = membership::prepare_start_activity(request, plan);
        break;
    case IngressAdapter::authorityResetAcknowledgement:
        return prepare_authority_reset_acknowledgement(
            binding, rosterDecode, adapter, request, plan, hasTransaction);
    case IngressAdapter::authorityAbdicate:
        return prepare_authority_abdication(
            binding, rosterDecode, adapter, request, plan, hasTransaction);
    case IngressAdapter::peerLeave:
        return prepare_peer_leave(binding, rosterDecode, adapter, request, plan, hasTransaction);
    case IngressAdapter::authorityRequestPurge:
        return prepare_authority_purge(
            binding, rosterDecode, adapter, request, plan, hasTransaction);
    case IngressAdapter::authorityQueryAnswer:
        return prepare_authority_query_answer(
            binding, rosterDecode, adapter, request, plan, hasTransaction);
    default:
        return frame_only(binding, rosterDecode, adapter, request);
    }
    // A message that cannot be staged is reported and dropped. Failing the frame would leave the
    // client's pending ring jammed.
    if (!prepared) {
        report_message(request.messageType, request.sessionId, "prepare");
        const state::activity::SessionBinding* const ownedBinding =
            acquiresBinding && !currentOwnsEnvelope ? nullptr : &binding.session;
        DiagnosticBody diagnostic{};
        diagnostic.status = server::activity::host::ClientMessageStatus::prepareRefused;
        static_cast<void>(record(request,
                                 store::Verdict::malformed,
                                 0,
                                 ownedBinding,
                                 binding.bindingGeneration,
                                 diagnostic,
                                 zeroHandlePatchEpoch));
        plan = {};
        return true;
    }
    if (adapter == IngressAdapter::joinRequestStateJoin) {
        const std::size_t consumedBits =
            request.payload.size() * middleware::encoding::kBitsPerByte;
        const Fingerprint fingerprint = payload_fingerprint(request.messageType, request.payload);
        plan.joinIngress.payloadFingerprint = fingerprint.value;
        plan.joinIngress.payloadBytes = static_cast<std::uint32_t>(request.payload.size());
        plan.joinIngress.peerHeardMask = request.peerHeardMask;
        plan.joinIngress.consumedBits = static_cast<std::uint32_t>(consumedBits);
        plan.joinIngress.hasPayloadFingerprint = fingerprint.present;
        plan.joinIngress.prepared = true;
        static_cast<void>(record(request, store::Verdict::framed, consumedBits, nullptr, 0));
        hasTransaction = true;
        return true;
    }
    const state::activity::SessionBinding& ownedBinding = binding.session;
    DiagnosticBody diagnostic{};
    diagnostic.status = server::activity::host::ClientMessageStatus::prepared;
    const bool publishesClientState = adapter == IngressAdapter::clientAuthoritativeDataMembership;
    if (publishesClientState) {
        diagnostic.authoritative = &plan.membershipMutation.authoritativeInput;
        diagnostic.consumedBits = request.payload.size() * middleware::encoding::kBitsPerByte;
        diagnostic.status = server::activity::host::ClientMessageStatus::decoded;
    }
    const std::uint64_t clientMessageSequence =
        record(request,
               store::Verdict::framed,
               request.payload.size() * middleware::encoding::kBitsPerByte,
               &ownedBinding,
               binding.bindingGeneration,
               diagnostic,
               zeroHandlePatchEpoch);
    if (adapter == IngressAdapter::entitySlotRequestStateSlots && clientMessageSequence != 0) {
        plan.entitySlotsRequested.binding = ownedBinding;
        plan.entitySlotsRequested.sourceGeneration = binding.bindingGeneration;
        plan.entitySlotsRequested.clientMessageSequence = clientMessageSequence;
        plan.entitySlotsRequested.pending = true;
    }
    if (publishesClientState && clientMessageSequence != 0) {
        plan.clientState.binding = ownedBinding;
        plan.clientState.sourceGeneration = binding.bindingGeneration;
        plan.clientState.clientMessageSequence = clientMessageSequence;
        plan.clientState.payloadBytes = static_cast<std::uint32_t>(request.payload.size());
        plan.clientState.pending = true;
    }
    hasTransaction = true;
    return true;
}

} // namespace sunrise::server::bap::encrypted::activity_message
