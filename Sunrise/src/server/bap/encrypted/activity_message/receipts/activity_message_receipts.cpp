/**
 * Framing handlers for every activity message that changes no State. Each one reads as much of its
 * body as the recovered grammar reaches, reports what it saw, and returns how completely the body
 * was read so the caller can record one arrival receipt. None of them acts on what it read.
 */

#include "activity_message_receipts.h"

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/bap/activity_message/activity_client_keepalive_validator.h"
#include "../../../../../middleware/bap/activity_message/entity_authority.h"
#include "../../../../../middleware/bap/activity_message/incident.h"
#include "../../../../../middleware/bap/activity_message/peer_ledger.h"
#include "../../../../../middleware/bap/activity_message/sense_update.h"
#include "../../../../../middleware/bap/activity_message/start_activity.h"
#include "../../../../../middleware/bap/activity_message/telemetry.h"
#include "../../../../../middleware/encoding/byte_order.h"

namespace sunrise::server::bap::encrypted::activity_message::receipts {
namespace {

namespace store = state::activity::receipts;
namespace authority = message::entity_authority;
namespace ledger = message::peer_ledger;
namespace telemetry = message::telemetry;

using store::Verdict;

/**
 * Writes one bounded key-value event on the server channel.
 * @param level Severity, checked against the channel threshold before formatting.
 * @param format Printf-style format holding one event line.
 */
void report(core::log::Level level, const char* format, ...) noexcept {
    if (!core::log::accepts(core::log::Channel::server, level)) {
        return;
    }
    std::array<char, core::log::kLineCapacity> line{};
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(line.data(), line.size(), format, arguments);
    va_end(arguments);
    if (written <= 0) {
        return;
    }
    // vsnprintf reports the length it wanted, so a truncated line reports past the buffer.
    const auto length = static_cast<std::size_t>(written) < line.size()
                            ? static_cast<std::size_t>(written)
                            : line.size() - 1;
    core::log::write(core::log::Channel::server, level, {line.data(), length});
}

/** @return The whole payload's bit count, which is the bar a fully framed body reaches. */
[[nodiscard]] std::size_t payload_bits(const message::Request& request) noexcept {
    return request.payload.size() * middleware::encoding::kBitsPerByte;
}

/**
 * Reports one body whose declared framing did not hold.
 * @param stage Short stable stage name for the log line.
 * @param request Validated envelope.
 * @return Always malformed, so the caller can return it directly.
 */
[[nodiscard]] Verdict report_malformed(const char* stage,
                                       const message::Request& request) noexcept {
    report(core::log::Level::warn,
           "ev=activity stage=%s result=malformed type=%u bytes=%zu",
           stage,
           request.messageType,
           request.payload.size());
    return Verdict::malformed;
}

} // namespace

/** Frames a sensor sense update and reports its epoch. */
Framed frame_sense_update(const message::Request& request) noexcept {
    namespace sense = message::sense_update;
    sense::SenseUpdate update{};
    std::size_t consumed = 0;
    if (!sense::parse_sense_update(request.payload, update, consumed)) {
        return {report_malformed("sense", request), consumed};
    }
    report(core::log::Level::debug,
           "ev=activity stage=sense result=framed epoch=0x%016llX%016llX groups_bits=%u",
           static_cast<unsigned long long>(update.epoch.first),
           static_cast<unsigned long long>(update.epoch.second),
           update.tailBits);
    // The group loop behind the sense delta has no recovered width, so the body is retained
    // rather than walked.
    return {update.tailBits == 0 ? Verdict::framed : Verdict::partial, consumed};
}

/** Records a service-8 envelope carrying the local-only activity-host request type. */
Framed frame_route_misuse(const message::Request& request) noexcept {
    // This type is a client-local message the transport turns into its own service. Arriving here
    // it is an authenticated but invalid route use, and answering it would allocate a second
    // session for one the client already has.
    report(core::log::Level::warn,
           "ev=activity stage=route result=misuse type=%u bytes=%zu",
           request.messageType,
           request.payload.size());
    return {Verdict::quarantined, 0};
}

/** Frames a start-new-activity request without applying any transition policy to it. */
Framed frame_start_activity(const message::Request& request) noexcept {
    namespace start = message::start_activity;
    start::StartActivity parsed{};
    std::size_t consumed = 0;
    if (!start::parse_start_activity(request.payload, parsed, consumed)) {
        return {report_malformed("start_activity", request), consumed};
    }
    const auto& continuation = parsed.continuation;
    const int packageLength = parsed.hasContinuation && continuation.hasPackageName
                                  ? static_cast<int>(continuation.packageNameLength)
                                  : 0;
    report(core::log::Level::info,
           "ev=activity stage=start_activity result=read from=%d to=%d tail=%u "
           "continuation=%u bubble=0x%X spawn=0x%X package=%.*s",
           parsed.sourceActivityIndex,
           parsed.destinationActivityIndex,
           parsed.tailBits,
           parsed.hasContinuation ? 1U : 0U,
           parsed.hasContinuation && continuation.hasArrivalBubbleHash
               ? continuation.arrivalBubbleHash
               : 0U,
           parsed.hasContinuation && continuation.hasSpawnSetHash ? continuation.spawnSetHash : 0U,
           packageLength,
           reinterpret_cast<const char*>(continuation.packageName.data()));
    return {parsed.tailBits == 0 ? Verdict::framed : Verdict::partial, consumed};
}

/** Frames a peer-reservation request as far as its revision. */
Framed frame_reservation_request(const message::Request& request) noexcept {
    telemetry::ReservationRequest parsed{};
    std::size_t consumed = 0;
    if (!telemetry::parse_reservation_request(request.payload, parsed, consumed)) {
        return {report_malformed("reservation", request), consumed};
    }
    report(core::log::Level::debug,
           "ev=activity stage=reservation result=read revision=%u records=%u",
           parsed.revision,
           parsed.recordBytes);
    const std::size_t tail =
        static_cast<std::size_t>(parsed.recordBytes) * middleware::encoding::kBitsPerByte;
    return {tail == 0 ? Verdict::framed : Verdict::partial, consumed};
}

/** Frames a reservation release. */
Framed frame_reservation_release(const message::Request& request) noexcept {
    ledger::ReservationRelease release{};
    std::size_t consumed = 0;
    if (!ledger::parse_release(request.payload, release, consumed)) {
        return {report_malformed("reservation_release", request), consumed};
    }
    report(core::log::Level::debug,
           "ev=activity stage=reservation_release result=read peer=0x%016llX",
           static_cast<unsigned long long>(release.peerKey));
    return {Verdict::framed, consumed};
}

/** Frames a peer leave notice. */
Framed frame_peer_leave(const message::Request& request) noexcept {
    ledger::PeerLeave leave{};
    std::size_t consumed = 0;
    if (!ledger::parse_leave(request.payload, leave, consumed)) {
        return {report_malformed("peer_leave", request), consumed};
    }
    report(core::log::Level::info,
           "ev=activity stage=peer_leave result=read peer=0x%016llX",
           static_cast<unsigned long long>(leave.peerKey));
    return {Verdict::framed, consumed};
}

/** Records a debug command without reading or running it. */
Framed frame_debug_command(const message::Request& request) noexcept {
    // The nested command definition is runtime selected, so the body cannot be walked from the
    // outer root alone. It is never executed, dispatched, or sent on to another client.
    report(core::log::Level::warn,
           "ev=activity stage=debug_command result=quarantined bytes=%zu",
           request.payload.size());
    return {Verdict::quarantined, 0};
}

/** Frames a connectivity failure report. */
Framed frame_connectivity_failure(const message::Request& request) noexcept {
    ledger::ConnectivityFailure failure{};
    std::size_t consumed = 0;
    if (!ledger::parse_connectivity_failure(request.payload, failure, consumed)) {
        return {report_malformed("connectivity", request), consumed};
    }
    report(core::log::Level::info,
           "ev=activity stage=connectivity result=read peer=0x%016llX reason=%u",
           static_cast<unsigned long long>(failure.peerKey),
           static_cast<unsigned>(failure.rawReason));
    // The two bits are the last schema field; the rest of the ninth byte is padding.
    return {Verdict::framed, consumed};
}

/** Records a client heartbeat as a bounded body. */
Framed frame_heartbeat(const message::Request& request) noexcept {
    // One runtime-selected nested definition, so the declared service length is the only bound.
    report(core::log::Level::debug,
           "ev=activity stage=heartbeat result=bounded bytes=%zu",
           request.payload.size());
    return {Verdict::partial, 0};
}

/** Frames a lag-switch report as far as its record count. */
Framed frame_lag_switch(const message::Request& request) noexcept {
    telemetry::LagSwitchReport parsed{};
    std::size_t consumed = 0;
    if (!telemetry::parse_lag_switch(request.payload, parsed, consumed)) {
        return {report_malformed("lag_switch", request), consumed};
    }
    report(parsed.aboveSupported ? core::log::Level::warn : core::log::Level::debug,
           "ev=activity stage=lag_switch result=%s records=%u tail=%u",
           parsed.aboveSupported ? "over_supported" : "read",
           static_cast<unsigned>(parsed.recordCount),
           parsed.recordBits);
    // A count above what the record grammar supports is retained and not acted on, because the
    // records behind it have no recovered shape either way.
    return {parsed.aboveSupported ? Verdict::quarantined : Verdict::partial, consumed};
}

/** Records a connection-quality report as a bounded body. */
Framed frame_connection_quality(const message::Request& request) noexcept {
    // Two nested structures whose leaf grammar is unresolved.
    report(core::log::Level::debug,
           "ev=activity stage=connection_quality result=bounded bytes=%zu",
           request.payload.size());
    return {Verdict::partial, 0};
}

/** Frames a speculative migration proposal without acting on it. */
Framed frame_migration(const message::Request& request) noexcept {
    ledger::MigrationProposal proposal{};
    std::size_t consumed = 0;
    if (!ledger::parse_migration(request.payload, proposal, consumed)) {
        return {report_malformed("migration", request), consumed};
    }
    // Host ownership never moves from a proposal. Acting on one needs the group migration state
    // machine, and a host that answers without it can split the session in two.
    report(core::log::Level::info,
           "ev=activity stage=migration result=noted peer=0x%016llX scalar=%d",
           static_cast<unsigned long long>(proposal.peerKey),
           proposal.scalar);
    return {Verdict::framed, consumed};
}

/** Frames the fixed high-water telemetry block. */
Framed frame_high_water(const message::Request& request) noexcept {
    telemetry::HighWater block{};
    std::size_t consumed = 0;
    if (!telemetry::parse_high_water(request.payload, block, consumed)) {
        return {report_malformed("high_water", request), consumed};
    }
    report(
        core::log::Level::debug, "ev=activity stage=high_water result=framed bits=%zu", consumed);
    return {Verdict::framed, consumed};
}

/** Frames one of the two opaque scalar messages. */
Framed frame_opaque_scalar(const message::Request& request) noexcept {
    std::int32_t value = 0;
    std::size_t consumed = 0;
    if (!telemetry::parse_opaque_scalar(request.payload, value, consumed)) {
        return {report_malformed("scalar", request), consumed};
    }
    report(core::log::Level::debug,
           "ev=activity stage=scalar result=read type=%u value=%d",
           request.messageType,
           value);
    return {Verdict::framed, consumed};
}

/** Frames the one-byte activity keepalive. */
Framed frame_client_keepalive(const message::Request& request) noexcept {
    namespace keepalive = message::client_keepalive;
    if (!keepalive::validate_client_keepalive(request.payload)) {
        return {report_malformed("keepalive", request), 0};
    }
    // The single byte is uninitialized at the sender, so it carries no value to read.
    const std::size_t consumed = payload_bits(request);
    return {Verdict::framed, consumed};
}

/** Frames one incident and quarantines a poison target. */
Framed frame_incident(const message::Request& request) noexcept {
    namespace incident = message::incident;
    incident::Incident parsed{};
    const incident::Verdict verdict = incident::validate(request.payload, parsed);
    const bool accepted = verdict == incident::Verdict::accepted;
    report(accepted ? core::log::Level::debug : core::log::Level::warn,
           "ev=activity stage=incident result=%s target=%u extra=%u selector=%u "
           "optional=%u payload=%u",
           incident::verdict_name(verdict),
           parsed.primaryTarget,
           parsed.extraTargetCount,
           parsed.selectorLength,
           static_cast<unsigned>(parsed.hasOptionalBlock),
           parsed.payloadLength);
    if (!accepted) {
        // A refused target index would index the consumer's table unbounded, so the body is kept
        // and never relayed.
        const Verdict outcome = verdict == incident::Verdict::targetPoisoned
                                        || verdict == incident::Verdict::targetOutOfRange
                                    ? Verdict::quarantined
                                    : Verdict::malformed;
        return {outcome, parsed.consumedBits};
    }
    return {Verdict::framed, parsed.consumedBits};
}

/** Frames one authority release, which records authority and returns no lease. */
Framed frame_authority_release(const message::Request& request, bool expectReason) noexcept {
    authority::Release decoded{};
    const bool parsed = expectReason ? authority::parse_abandon(request.payload, decoded)
                                     : authority::parse_abdicate(request.payload, decoded);
    if (!parsed) {
        return {report_malformed("authority", request), 0};
    }
    report(core::log::Level::debug,
           "ev=activity stage=authority result=noted type=%u selector=%u reason=%d",
           request.messageType,
           static_cast<unsigned>(decoded.selector),
           decoded.hasReason ? decoded.reason : 0);
    return {Verdict::framed, payload_bits(request)};
}

/** Frames one purge request. Nothing answers it. */
Framed frame_request_purge(const message::Request& request) noexcept {
    std::int32_t reason = 0;
    if (!authority::parse_request_purge(request.payload, reason)) {
        return {report_malformed("purge", request), 0};
    }
    // The answer would have to name the exact next authority generation, which nothing here
    // tracks, and the consumer asserts on any other value.
    report(core::log::Level::debug, "ev=activity stage=purge result=noted reason=%d", reason);
    return {Verdict::framed, payload_bits(request)};
}

/** Frames one authority query answer. */
Framed frame_query_answer(const message::Request& request) noexcept {
    authority::QueryAnswer answer{};
    if (!authority::parse_query_answer(request.messageType, request.payload, answer)) {
        return {report_malformed("authority_answer", request), 0};
    }
    // This host sends no query, so an answer is the client reconciling on its own.
    report(core::log::Level::debug,
           "ev=activity stage=authority result=answer type=%u corr=0x%08X selector=%d",
           request.messageType,
           answer.correlation,
           answer.hasSelector ? static_cast<int>(answer.selector) : -1);
    return {Verdict::framed, payload_bits(request)};
}

/** Records an envelope whose message type has no recovered body grammar. */
Framed frame_unknown(const message::Request& request) noexcept {
    report(core::log::Level::warn,
           "ev=activity stage=unknown result=bounded type=%u bytes=%zu",
           request.messageType,
           request.payload.size());
    return {Verdict::partial, 0};
}

} // namespace sunrise::server::bap::encrypted::activity_message::receipts
