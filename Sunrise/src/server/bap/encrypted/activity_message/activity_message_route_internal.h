#pragma once

#include <cstddef>
#include <cstdint>

#include "../../../../middleware/bap/activity_message/activity_message_request_parser.h"
#include "../../../../middleware/bap/activity_message/sense_update.h"
#include "../../../../middleware/bap/activity_message/wire_schema/activity_communication_route.h"
#include "../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../state/activity/receipts/activity_receipts.h"
#include "../../../../state/activity/runtime.h"
#include "../../../activity/host_runtime.h"
#include "activity_message_route.h"
#include "receipts/activity_message_receipts.h"

namespace sunrise::server::bap::encrypted::activity_message {

namespace service = middleware::bap::activity_message;
namespace store = state::activity::receipts;
namespace communication = service::wire_schema::communication;
using IngressAdapter = communication::IngressAdapter;

/** Diagnostic facts kept separately from the aggregate receipt verdict. */
struct DiagnosticBody final {
    const state::activity::membership::AuthoritativeUpdate* authoritative{};
    const service::sense_update::DecodedPacket* sense{};
    std::size_t consumedBits{};
    server::activity::host::ClientMessageStatus status{
        server::activity::host::ClientMessageStatus::unclassified};
};

/**
 * Reports one activity message the route did not stage, naming its type.
 * Every inbound activity message is one-way, so nothing here can jam the client's reply ring. An
 * unnamed drop is invisible, and membership waits on the identity message.
 * @param messageType Activity message type from the envelope.
 * @param sessionId Activity session the envelope named.
 * @param reason Short name of the step that declined.
 */
void report_message(std::uint32_t messageType,
                    std::uint64_t sessionId,
                    const char* reason) noexcept;

/**
 * Maps one framing-only parser result onto a diagnostic body status.
 * @param framed Framing verdict and the bits it consumed.
 * @param incident True for the incident adapter, which frames the outer body alone.
 * @return Status the owned diagnostic history records.
 */
[[nodiscard]] server::activity::host::ClientMessageStatus
diagnostic_status(const receipts::Framed& framed, bool incident) noexcept;

/**
 * Records one arrival in the aggregate receipts and the owned diagnostic history.
 * @param request Validated envelope.
 * @param receiptVerdict Aggregate receipt verdict for this arrival.
 * @param receiptConsumedBits Bits the framing parser read from the body.
 * @param ownedBinding Exact owner, or null when the message was not owned.
 * @param sourceGeneration Link generation that owns the binding.
 * @param diagnostic Extra facts kept only for an owned arrival.
 * @param zeroHandleOwned True only for retail msg52 after its current link binding was proved.
 * @return Client message sequence, or zero when no owned row was recorded.
 */
[[nodiscard]] std::uint64_t record(const service::Request& request,
                                   store::Verdict receiptVerdict,
                                   std::size_t receiptConsumedBits,
                                   const state::activity::SessionBinding* ownedBinding,
                                   std::uint64_t sourceGeneration,
                                   const DiagnosticBody& diagnostic = {},
                                   bool zeroHandleOwned = false) noexcept;

/**
 * Frames one message and records its receipt.
 * @param binding Exact ActivityClient generation owned by this link.
 * @param rosterDecode Last complete msg-5 identity map delivered on this same link.
 * @param adapter Ingress adapter the communication route named for this message type.
 * @param request Validated envelope.
 * @return Always true: a framing-only message can never fail the transport frame.
 */
[[nodiscard]] bool frame_only(const ActivityClientBinding& binding,
                              const RosterDecodeMap& rosterDecode,
                              IngressAdapter adapter,
                              const service::Request& request) noexcept;

/**
 * Retains one exact msg-31 or msg-32 answer until its authenticated frame commits.
 * @param binding Exact ActivityClient generation owned by this link.
 * @param rosterDecode Last complete msg-5 identity map delivered on this same link.
 * @param adapter Ingress adapter the communication route named for this message type.
 * @param request Validated owned svc8 envelope.
 * @param plan Cleared, then receives the retained answer.
 * @param hasTransaction Receives true only when the answer stages a transaction.
 * @return Always true: the body is framed before any retention decision.
 */
[[nodiscard]] bool prepare_authority_query_answer(const ActivityClientBinding& binding,
                                                  const RosterDecodeMap& rosterDecode,
                                                  IngressAdapter adapter,
                                                  const service::Request& request,
                                                  ActivityPlan& plan,
                                                  bool& hasTransaction) noexcept;

/**
 * Stages the leave delta activity msg 15 asks for.
 * @param binding Exact ActivityClient generation owned by this link.
 * @param rosterDecode Last complete msg-5 identity map delivered on this same link.
 * @param adapter Ingress adapter the communication route named for this message type.
 * @param request Validated owned svc8 envelope.
 * @param plan Cleared, then receives the leave notification.
 * @param hasTransaction Receives true only when a roster was published on this link.
 * @return Always true: the body is framed before any answer decision.
 */
[[nodiscard]] bool prepare_peer_leave(const ActivityClientBinding& binding,
                                      const RosterDecodeMap& rosterDecode,
                                      IngressAdapter adapter,
                                      const service::Request& request,
                                      ActivityPlan& plan,
                                      bool& hasTransaction) noexcept;

/**
 * Retains one exact msg-27 purge request with its complete mask and reason.
 * @param binding Exact ActivityClient generation owned by this link.
 * @param rosterDecode Last complete msg-5 identity map delivered on this same link.
 * @param adapter Ingress adapter the communication route named for this message type.
 * @param request Validated owned svc8 envelope.
 * @param plan Cleared, then receives the retained purge body and its notification.
 * @param hasTransaction Receives true only when the request stages a transaction.
 * @return Always true: the body is framed before any retention decision.
 */
[[nodiscard]] bool prepare_authority_purge(const ActivityClientBinding& binding,
                                           const RosterDecodeMap& rosterDecode,
                                           IngressAdapter adapter,
                                           const service::Request& request,
                                           ActivityPlan& plan,
                                           bool& hasTransaction) noexcept;

/**
 * Retains one exact msg-33 abdication or msg-26 abandonment until its authenticated frame commits.
 * @param binding Exact ActivityClient generation owned by this link.
 * @param rosterDecode Last complete msg-5 identity map delivered on this same link.
 * @param adapter Ingress adapter the communication route named for this message type; the
 * abandon adapter selects msg 26.
 * @param request Validated owned svc8 envelope.
 * @param plan Cleared, then receives the released mask and its bubble.
 * @param hasTransaction Receives true only when the abdication stages a transaction.
 * @return Always true: the body is framed before any retention decision.
 */
[[nodiscard]] bool prepare_authority_abdication(const ActivityClientBinding& binding,
                                                const RosterDecodeMap& rosterDecode,
                                                IngressAdapter adapter,
                                                const service::Request& request,
                                                ActivityPlan& plan,
                                                bool& hasTransaction) noexcept;

/**
 * Retains one exact msg-29 acknowledgement until its authenticated frame commits.
 * @param binding Exact ActivityClient generation owned by this link.
 * @param rosterDecode Last complete msg-5 identity map delivered on this same link.
 * @param adapter Ingress adapter the communication route named for this message type.
 * @param request Validated owned svc8 envelope.
 * @param plan Cleared, then receives the retained acknowledgement.
 * @param hasTransaction Receives true only when the acknowledgement stages a transaction.
 * @return Always true: the body is framed before any retention decision.
 */
[[nodiscard]] bool prepare_authority_reset_acknowledgement(const ActivityClientBinding& binding,
                                                           const RosterDecodeMap& rosterDecode,
                                                           IngressAdapter adapter,
                                                           const service::Request& request,
                                                           ActivityPlan& plan,
                                                           bool& hasTransaction) noexcept;

} // namespace sunrise::server::bap::encrypted::activity_message
