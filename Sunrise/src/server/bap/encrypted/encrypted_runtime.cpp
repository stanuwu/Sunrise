#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>

#include "../../../core/logging/log.h"
#include "../../../middleware/encoding/byte_order.h"
#include "../../../middleware/secure_channel/runtime.h"
#include "../../../middleware/web_service/messages/opcode206.h"
#include "../../../state/activity/bubble_authority/runtime.h"
#include "../../../state/runtime/runtime.h"
#include "../../activity/host_runtime.h"
#include "../../gameplay/peer/peer_transport.h"
#include "../../gameplay/squad_entity_retirement.h"
#include "../activity_authority_query_owner.h"
#include "../activity_authority_reset_owner.h"
#include "../internal.h"
#include "activity_transaction/activity_transaction_notifications.h"
#include "bap_connection_publication.h"
#include "internal.h"
#include "push/activity/activity_roster_push.h"
#include "queuez/queuez_outcome_staging.h"
#include "state/investment/store_internal.h"
#include "transactions/service_outcome_commit.h"

namespace sunrise::server::bap::encrypted {
namespace {

/** Delay before the Family-4 copy of an artifact change, so its Family-5 refresh lands first. */
constexpr std::uint64_t kArtifactFamily4RefreshDelayMs = 100;

/** Traces one decoded service frame and the reply it produced. */
void report_service_traffic(const middleware::bap::RequestFrame& frame,
                            const ServiceRoute& route,
                            std::size_t responseBodySize) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=bap stage=service svc=%u task=%u request_bytes=%zu "
                                      "response_svc=%u response_bytes=%zu",
                                      static_cast<unsigned>(frame.serviceId),
                                      static_cast<unsigned>(frame.taskId),
                                      frame.body.size(),
                                      static_cast<unsigned>(route.response),
                                      responseBodySize);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Wipes the part of one scratch buffer that may hold written bytes.
 * @param buffer Lock-owned scratch storage.
 * @param size Largest prefix that may hold transformed bytes.
 */
void clear_prefix(std::span<std::byte> buffer, std::size_t size) noexcept {
    SecureZeroMemory(buffer.data(), (std::min)(buffer.size(), size));
}

/** Appends one join only after its exact ActivityClient binding has committed and published. */
void record_committed_join(Session& session, const ConnectionFields& fields) noexcept {
    if (!fields.joinsActivity || !fields.joinIngress.prepared) {
        return;
    }
    server::activity::host::ClientMessageInput input{};
    input.binding = session.activity.session;
    input.sourceGeneration = session.activity.bindingGeneration;
    input.payloadFingerprint = fields.joinIngress.payloadFingerprint;
    input.messageType = 3;
    input.payloadBytes = fields.joinIngress.payloadBytes;
    input.peerHeardMask = fields.joinIngress.peerHeardMask;
    input.consumedBits = fields.joinIngress.consumedBits;
    input.hasPayloadFingerprint = fields.joinIngress.hasPayloadFingerprint;
    const std::uint64_t payloadBits =
        static_cast<std::uint64_t>(input.payloadBytes) * middleware::encoding::kBitsPerByte;
    input.status = payloadBits > input.consumedBits
                       ? server::activity::host::ClientMessageStatus::prefixOnly
                       : server::activity::host::ClientMessageStatus::decoded;
    static_cast<void>(server::activity::host::record_client_message(input));
}

/** Queues one safe msg-22 after-image only after State and connection publication commit. */
void submit_committed_client_state(const activity_message::ActivityPlan& plan,
                                   const transactions::Publication& publication) noexcept {
    // A committed msg 22 that changed nothing material still reaches the surface with every
    // delta field absent. The client sends such reports while loading as well as after the
    // spawn, so a script must not time its opening on one.
    if (!plan.clientState.pending || !publication.clientState.committed) {
        return;
    }
    server::activity::host::ClientStateChangeInput input{};
    input.binding = plan.clientState.binding;
    input.state = publication.clientState;
    input.sourceGeneration = plan.clientState.sourceGeneration;
    input.clientMessageSequence = plan.clientState.clientMessageSequence;
    input.payloadBytes = plan.clientState.payloadBytes;
    if (!server::activity::host::submit_client_state_change(input)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=activity stage=client_state_ingress result=refused");
    }
}

/** Queues exact entity-slot demand only after its grant transaction commits. */
void submit_committed_entity_slots_requested(const activity_message::ActivityPlan& plan) noexcept {
    if (!plan.entitySlotsRequested.pending) {
        return;
    }
    server::activity::host::EntitySlotsRequestedInput input{};
    input.binding = plan.entitySlotsRequested.binding;
    input.sourceGeneration = plan.entitySlotsRequested.sourceGeneration;
    input.clientMessageSequence = plan.entitySlotsRequested.clientMessageSequence;
    input.requestedCount = plan.entitySlotsRequested.requestedCount;
    if (!server::activity::host::submit_entity_slots_requested(input)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=activity stage=entity_slots_requested result=refused");
    }
}

/** Applies one query answer only after its authenticated service frame commits. */
void submit_committed_authority_answer(Session& session,
                                       const activity_message::ActivityPlan& plan) noexcept {
    if (!plan.authorityQuery.pending
        || plan.mutationDomain != activity_message::MutationDomain::authorityQuery) {
        return;
    }
    const authority_query::AnswerStatus status =
        authority_query::submit(session.activityAuthorityQuery,
                                plan.authorityQuery.sourceGeneration,
                                GetTickCount64(),
                                plan.authorityQuery.answer);
    if (status != authority_query::AnswerStatus::bubbleAccepted
        && status != authority_query::AnswerStatus::complete) {
        std::array<char, core::log::kLineCapacity> line{};
        const int count =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=activity stage=authority_query result=refused status=%u",
                          static_cast<unsigned>(status));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
    }
}

/** An old ActivityClient cannot relinquish the replacement client's grant. */
void submit_committed_authority_abdication(Session& session,
                                           const activity_message::ActivityPlan& plan) noexcept {
    if (plan.mutationDomain != activity_message::MutationDomain::authorityAbdication
        || !plan.authorityAbdication.pending
        || plan.authorityAbdication.sourceGeneration != session.activity.bindingGeneration
        || plan.sessionId != session.activity.session.sessionId) {
        return;
    }
    state::activity::bubble_authority::record_abdication(
        plan.sessionId, plan.authorityAbdication.bubble, &plan.authorityAbdication.entities);
    server::gameplay::squad_entity_retirement::observe_abdication(
        session.activity.session,
        session.activity.bindingGeneration,
        plan.authorityAbdication.bubble,
        plan.authorityAbdication.entities);
}

/** Applies one reset acknowledgement only after its authenticated service frame commits. */
void submit_committed_authority_reset(Session& session,
                                      const activity_message::ActivityPlan& plan) noexcept {
    if (!plan.authorityReset.pending
        || plan.mutationDomain != activity_message::MutationDomain::authorityReset) {
        return;
    }
    const authority_reset::AcknowledgementStatus status =
        authority_reset::submit(session.activityAuthorityReset,
                                plan.authorityReset.sourceGeneration,
                                GetTickCount64(),
                                plan.authorityReset.answer);
    if (status != authority_reset::AcknowledgementStatus::complete) {
        std::array<char, core::log::kLineCapacity> line{};
        const int count =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=activity stage=authority_reset result=refused status=%u",
                          static_cast<unsigned>(status));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
    }
}

} // namespace

/**
 * Authenticates and answers one supported encrypted post-bootstrap request.
 * @param session Connection-owned authentication and nonce state.
 * @param scratch Lock-owned transform buffers kept off the Client thread stack.
 * @param outer Validated encrypted outer frame.
 * @param response Caller-owned complete-frame storage.
 * @param written Receives encoded response bytes.
 * @return True when routing succeeds and any response fits, commits State, and publishes its nonce.
 */
bool consume(Session& session,
             Scratch& scratch,
             const middleware::bap::OuterFrame& outer,
             std::span<std::byte> response,
             std::size_t& written) noexcept {
    written = 0;
    session.accountMutationPublished = false;
    if (!session.authenticated) {
        // Staying silent here looks the same as a decode fault, and both look like a dead link.
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=bap stage=encrypted result=drop reason=unauthenticated");
        return false;
    }

    std::size_t plaintextSize = 0;
    if (!middleware::secure_channel::open_frame(session.sessionKey,
                                                session.receiveNonce,
                                                outer.payload,
                                                scratch.plaintext,
                                                plaintextSize)) {
        const std::size_t possiblePlaintextSize =
            outer.payload.size() >= middleware::secure_channel::kFrameTagSize
                ? outer.payload.size() - middleware::secure_channel::kFrameTagSize
                : 0;
        clear_prefix(scratch.plaintext, possiblePlaintextSize);
        // The service is unreadable while the frame is sealed, so this line names no service.
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=bap svc=none stage=decrypt result=fail");
        return false;
    }
    // Authentication consumes the receive nonce even when the inner service is unsupported.
    middleware::secure_channel::advance_nonce(session.receiveNonce);

    middleware::bap::RequestFrame frame;
    ServiceRoute route;
    std::size_t responseBodySize = 0;
    std::size_t framedSize = 0;
    ServiceOutcome outcome{};
    transactions::Publication publication{};
    queuez::SessionState nextQueuez = session.queuez;
    bool publishesQueuez = false;
    bool handled =
        middleware::bap::parse_request_payload(std::span(scratch.plaintext).first(plaintextSize),
                                               middleware::bap::FrameType::encrypted,
                                               frame)
        && routing::resolve(frame.serviceId, route);
    if (!handled) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=bap svc=none stage=parse result=fail");
    }
    const bool processesBody = handled && route.responseMode != ResponseMode::none;
    const bool sendsReply = handled && route.responseMode == ResponseMode::reply;
    bool staleWebAction = false;
    // An armed resync means this peer's published manifest predates the account it would name.
    if (processesBody && route.bodyCodec == BodyCodec::webService && session.accountResyncArmed
        && !web_service::encode_resident_dependent_refusal(
            frame.body, scratch.responseBody, responseBodySize, staleWebAction)) {
        handled = false;
        diagnostics::report_failure(frame.serviceId, "stale_refusal");
    }
    if (staleWebAction) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=bap stage=web_service result=refuse reason=stale_manifest");
    }
    state::investment::store::Transaction investmentTransaction;
    if (!investmentTransaction.ready()) {
        return false;
    }
    std::array<state::account::inventory::PresentedItemRow,
               queuez::kAcquisitionPresentationRowCapacity>
        presentation{};
    if (session.acquisitionPresentationRowCount > presentation.size()) {
        return false;
    }
    for (std::size_t index = 0; index < session.acquisitionPresentationRowCount; ++index) {
        const auto& row = session.acquisitionPresentationRows[index];
        presentation[index] = {row.instanceSoid, row.inventoryRow};
    }
    // Pure one-way services consume only the authenticated receive nonce.
    if (!staleWebAction && processesBody
        && !body::process(route,
                          session.queuez,
                          session.activity,
                          session.activityRosterDecode,
                          session.matchmakingContext,
                          frame.body,
                          scratch.responseBody,
                          responseBodySize,
                          outcome,
                          std::span(presentation).first(session.acquisitionPresentationRowCount))) {
        diagnostics::report_failure(frame.serviceId, "body");
        // A reply-mode service answers with an empty body instead of not at all. The Client
        // matches only the head of its pending ring. One unanswered request jams that ring for
        // good, and every later reply is rejected, which is worse than a thin reply.
        clear_prefix(scratch.responseBody, responseBodySize);
        responseBodySize = 0;
        outcome = {};
        handled = sendsReply;
    }
    // A ws-206 subscribe is answered with the family's first snapshot inside the reply. The
    // client creates the family from that blob and reads it with no null check.
    if (handled && sendsReply && outcome.hasSubscription
        && route.bodyCodec == BodyCodec::webService) {
        middleware::web_service::Message message;
        std::size_t bodySize = 0;
        outcome.subscriptionAnswered =
            middleware::web_service::parse_request(frame.body, message)
            && message.opcode == middleware::web_service::messages::opcode206::kOpcode
            && push::prepare_subscription_answer(
                scratch, session.queuez, outcome.subscription, scratch.responsePayload, bodySize)
            && middleware::web_service::messages::opcode206::encode_response(
                message,
                std::span(scratch.responsePayload).first(bodySize),
                scratch.responseBody,
                responseBodySize);
        clear_prefix(scratch.responsePayload, bodySize);
    }
    if (handled) {
        report_service_traffic(frame, route, responseBodySize);
    }
    if (handled && sendsReply) {
        handled = reply::encode(scratch,
                                route,
                                frame.taskId,
                                session.sessionKey,
                                session.sendNonce,
                                std::span(scratch.responseBody).first(responseBodySize),
                                framedSize);
        if (!handled) {
            diagnostics::report_failure(frame.serviceId, "encode");
        }
    }
    // Stage every requested frame and check for caller room before committing State or the nonce.
    auto nextSendNonce = session.sendNonce;
    if (handled && sendsReply) {
        middleware::secure_channel::advance_nonce(nextSendNonce);
    }
    queuez::StagedPublication queuezPublication{};
    if (handled) {
        const std::uint64_t now = GetTickCount64();
        if (now >= session.acquisitionPresentationUntilTick) {
            session.acquisitionPresentationRows = {};
            session.acquisitionPresentationRowCount = 0;
        }
        const auto acquisitionPresentationRows =
            std::span(session.acquisitionPresentationRows)
                .first(session.acquisitionPresentationRowCount);
        const bool preserveAcquisitionPresentation = now < session.acquisitionPresentationUntilTick;
        handled = queuez::stage_service_outcome(scratch,
                                                session.queuez,
                                                outcome,
                                                preserveAcquisitionPresentation,
                                                acquisitionPresentationRows,
                                                session.sessionKey,
                                                nextSendNonce,
                                                scratch.framed,
                                                framedSize,
                                                queuezPublication);
        if (handled && queuezPublication.hasState) {
            nextQueuez = queuezPublication.after;
            publishesQueuez = true;
        }
        if (!handled) {
            diagnostics::report_failure(frame.serviceId, "stage");
        }
    }
    const auto* activityPlan = transaction_if<activity_message::ActivityPlan>(outcome);
    if (handled && activityPlan != nullptr) {
        handled = route.responseMode == ResponseMode::uncorrelatedPush;
        if (!handled) {
            diagnostics::report_failure(frame.serviceId, "route");
        } else if (!activity_transaction::stage_notifications(session,
                                                              scratch,
                                                              *activityPlan,
                                                              session.sessionKey,
                                                              nextSendNonce,
                                                              scratch.framed,
                                                              framedSize)) {
            // Reports still commit their observed state; a failed snapshot answer commits nothing.
            diagnostics::report_failure(frame.serviceId, "notify");
            if (activityPlan->mutationDomain == activity_message::MutationDomain::authorityPurge
                || activityPlan->delivery == activity_message::Delivery::refreshNotifications) {
                handled = false;
            }
        }
    }
    const bool artifactPurchase = transaction_if<ArtifactPurchaseTransaction>(outcome) != nullptr;
    const bool vendorReputation =
        transaction_if<state::PendingVendorReputation>(outcome) != nullptr;
    const bool mutatesAccount =
        outcome.hasSelectCharacter || outcome.hasRecordClaim || outcome.hasArtifactReset
        || transaction_if<EquipmentSwapTransaction>(outcome) != nullptr
        || transaction_if<SubclassSelectionTransaction>(outcome) != nullptr
        || transaction_if<SocketPlugTransaction>(outcome) != nullptr
        || transaction_if<ItemStateTransaction>(outcome) != nullptr || artifactPurchase
        || transaction_if<ItemAcquisitionTransaction>(outcome) != nullptr
        || transaction_if<ProfileItemAcquisitionTransaction>(outcome) != nullptr
        || transaction_if<ItemDismantleTransaction>(outcome) != nullptr
        || transaction_if<RecordRewardGrantTransaction>(outcome) != nullptr
        || transaction_if<SeasonPassRewardTransaction>(outcome) != nullptr
        || transaction_if<state::PendingSettingsUpdate>(outcome) != nullptr || vendorReputation;
    const bool presentsAcquisition =
        transaction_if<ItemAcquisitionTransaction>(outcome) != nullptr
        || transaction_if<ProfileItemAcquisitionTransaction>(outcome) != nullptr
        || transaction_if<RecordRewardGrantTransaction>(outcome) != nullptr
        || transaction_if<SeasonPassRewardTransaction>(outcome) != nullptr;
    const bool invalidatesAcquisitionPresentation =
        outcome.hasChangeCharacter || outcome.hasSelectCharacter || outcome.hasArtifactReset
        || transaction_if<ItemDismantleTransaction>(outcome) != nullptr;
    const bool hasPrecommittedAccountAction =
        outcome.hasRecordClaim || outcome.hasSelectCharacter || outcome.hasArtifactReset;
    // Commit consumes pending payloads, so retain the connection fields first.
    const ConnectionFields connection = connection_fields(outcome);
    if (handled && processesBody) {
        // State changes become visible only after every requested frame and caller byte fit.
        // A refused commit sends nothing, so the frame's own reason is the only record of why.
        const char* commitReason = "none";
        server::gameplay::entity_identities::PublicationLease entityLease;
        const bool retirementValid =
            push::activity::begin_staged_roster_publication(session, entityLease);
        const bool fits = framedSize <= response.size();
        if (!retirementValid) {
            commitReason = "entity_retirement_stale";
        }
        handled = fits && retirementValid
                  && transactions::commit(outcome, publication, commitReason)
                  && investmentTransaction.commit();
        if (!handled) {
            diagnostics::report_failure(
                frame.serviceId, "commit", fits ? commitReason : "frame_capacity");
        }
        if (handled) {
            std::copy_n(scratch.framed.begin(), framedSize, response.begin());
            written = framedSize;
            entityLease.release();
            // The caller copy finishes before connection fields are published.
            session.sendNonce = nextSendNonce;
            if (publishesQueuez) {
                session.queuez = nextQueuez;
            }
            arm_repushes(session, queuezPublication);
            if (invalidatesAcquisitionPresentation) {
                session.acquisitionPresentationRows = {};
                session.acquisitionPresentationRowCount = 0;
                session.acquisitionPresentationUntilTick = 0;
            } else if (queuezPublication.updatesAcquisitionPresentationRows) {
                session.acquisitionPresentationRows = queuezPublication.acquisitionPresentationRows;
                session.acquisitionPresentationRowCount =
                    queuezPublication.acquisitionPresentationRowCount;
                if (session.acquisitionPresentationRowCount == 0) {
                    session.acquisitionPresentationUntilTick = 0;
                }
            }
            if (presentsAcquisition && publishesQueuez) {
                bap::arm_acquisition_presentation_hold(session);
            }
            publish_connection_fields(session, publication, connection);
            record_committed_join(session, connection);
            // The caller copy is done, so what the staged roster body owes is settled here.
            push::activity::commit_staged_roster(session);
            commit_staged_advertisement(session);
            if (activityPlan != nullptr) {
                submit_committed_client_state(*activityPlan, publication);
                submit_committed_entity_slots_requested(*activityPlan);
                submit_committed_authority_reset(session, *activityPlan);
                submit_committed_authority_answer(session, *activityPlan);
                submit_committed_authority_abdication(session, *activityPlan);
                if (activityPlan->hasReturnedEntitySlots
                    && activityPlan->sessionId == session.activity.session.sessionId) {
                    server::gameplay::squad_entity_retirement::returned_slots(
                        session.activity.session,
                        session.activity.bindingGeneration,
                        activityPlan->returnedEntitySlots);
                }
                if (activityPlan->mutationDomain
                    == activity_message::MutationDomain::authorityPurge) {
                    const auto previousEpoch = session.activity.replicationEpoch;
                    session.activity.replicationEpoch = activityPlan->authorityPurge.body.epoch;
                    const auto updatedViews = server::gameplay::peer::commit_replication_epoch(
                        session.activity.session,
                        session.activity.bindingGeneration,
                        previousEpoch,
                        session.activity.replicationEpoch);
                    state::activity::bubble_authority::record_purge(
                        activityPlan->sessionId, activityPlan->authorityPurge.body.slots);
                    server::gameplay::squad_entity_retirement::returned_slots(
                        session.activity.session,
                        session.activity.bindingGeneration,
                        activityPlan->authorityPurge.body.slots);
                    if (session.activityReplicationEpoch.pending
                        && session.activityReplicationEpoch.generation
                               == session.activity.replicationEpoch) {
                        session.activityReplicationEpoch.pending = false;
                    }
                    unsigned slots = 0;
                    for (const std::byte byte : activityPlan->authorityPurge.body.slots) {
                        slots +=
                            static_cast<unsigned>(std::popcount(std::to_integer<unsigned>(byte)));
                    }
                    std::array<char, core::log::kLineCapacity> line{};
                    const int count =
                        std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=purge result=published epoch=%u reason=%d "
                                      "slots=%u views=%zu",
                                      static_cast<unsigned>(session.activity.replicationEpoch),
                                      static_cast<int>(activityPlan->authorityPurge.body.reason),
                                      slots,
                                      updatedViews);
                    if (count > 0) {
                        core::log::write(core::log::Channel::server,
                                         core::log::Level::debug,
                                         {line.data(), static_cast<std::size_t>(count)});
                    }
                }
            }
            // Any delivered activity notification resets the client's silence timer, so the
            // fallback keepalive is delayed. A roster-only answer is excluded: it owes neither the
            // global state nor the membership, and at once a second it would starve the keepalive.
            const bool defersKeepalive =
                activityPlan != nullptr && framedSize != 0
                && activityPlan->delivery != activity_message::Delivery::rosterNotification;
            if (defersKeepalive) {
                session.activityKeepaliveDueTick = GetTickCount64() + kActivityKeepaliveIntervalMs;
            }
            const bool resyncsCommittedAccount =
                vendorReputation || (hasPrecommittedAccountAction && !queuezPublication.hasState);
            if (resyncsCommittedAccount) {
                bap::arm_account_resync_everywhere();
            }
            if (artifactPurchase || outcome.hasArtifactReset) {
                // Artifact overrides live in Family 5, so they need their own refresh. A record
                // claim does not: its Family-4 replacement rearms the client rebuild.
                session.artifactRefreshArmed = true;
                session.artifactFamily4RefreshDueTick =
                    GetTickCount64() + kArtifactFamily4RefreshDelayMs;
                session.artifactFamily4RefreshArmed = true;
            }
            if (outcome.hasArtifactReset) {
                session.artifactResetRefresh = outcome.artifactReset;
                session.artifactResetRefreshCursor = 0;
            }
            session.accountMutationPublished = mutatesAccount && !resyncsCommittedAccount;
        }
    }
    if (!handled) {
        if (hasPrecommittedAccountAction) {
            bap::arm_account_resync_everywhere();
        }
        // The staged body is dropped, so its grant and its state byte go back for the next push.
        push::activity::discard_staged_roster(session);
        discard_staged_advertisement(session);
    }
    clear_prefix(scratch.plaintext, plaintextSize);
    clear_prefix(scratch.responseBody, responseBodySize);
    clear_prefix(scratch.framed, framedSize);
    outcome = {};
    SecureZeroMemory(&publication, sizeof publication);
    SecureZeroMemory(&queuezPublication, sizeof queuezPublication);
    return handled;
}

} // namespace sunrise::server::bap::encrypted
