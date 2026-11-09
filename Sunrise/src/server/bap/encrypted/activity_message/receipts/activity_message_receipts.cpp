/**
 * Framing handlers for every activity message. None of them changes State.
 * Each one reads as much of its body as the known grammar reaches, and reports what it saw.
 * It returns how completely the body was read, so the caller can record one arrival receipt.
 * An accepted incident is handed to the grant applier, which owns every state change.
 */

#include "activity_message_receipts.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/bap/activity_message/activity_client_keepalive_validator.h"
#include "../../../../../middleware/bap/activity_message/entity_authority.h"
#include "../../../../../middleware/bap/activity_message/incident.h"
#include "../../../../../middleware/bap/activity_message/peer_ledger.h"
#include "../../../../../middleware/bap/activity_message/scriptable_auth_body.h"
#include "../../../../../middleware/bap/activity_message/sense_update.h"
#include "../../../../../middleware/bap/activity_message/squad_sense_state.h"
#include "../../../../../middleware/bap/activity_message/start_activity.h"
#include "../../../../../middleware/bap/activity_message/telemetry.h"
#include "../../../../../middleware/encoding/byte_order.h"
#include "../../../../../state/activity_sdk/format.h"
#include "../../../../activity/mission/mission_script_actor_path_sense.h"
#include "../../../../activity/mission/mission_script_squad_sense.h"
#include "activity_incident_grants.h"
#include "activity_kill_progress.h"

namespace sunrise::server::bap::encrypted::activity_message::receipts {
namespace {

namespace store = state::activity::receipts;
namespace authority = message::entity_authority;
namespace ledger = message::peer_ledger;
namespace telemetry = message::telemetry;
namespace sense = message::sense_update;

using store::Verdict;

/** Type-4 Sense has seven fixed prefix values when it carries no predicate replies. */
constexpr std::uint32_t kObjectSenseSchema = state::activity_sdk::format::kObjectSenseSchema;
constexpr std::uint32_t kObjectSenseValueCount = 7;

/** Type-2 Sense reports the atom runner's progress, so its value count varies with the body. */
constexpr std::uint32_t kCombatantSenseSchema = message::scriptable_auth::kType2SenseSchema;
constexpr std::uint32_t kCombatantAtomsSchema = server::activity::mission::kCombatantProgramSchema;

/** Type-1 Sense carries the alive count and the eight per-slot member counts. */
constexpr std::uint32_t kSquadSenseSchema = message::squad_sense::kSchema;
constexpr std::uint32_t kSquadSlotsSchema = server::activity::mission::kSquadCreatedListSchema;
constexpr std::uint32_t kSquadCountsSchema = server::activity::mission::kSquadCreatedCountSchema;
constexpr std::uint32_t kSquadSlotCapacity = 8;

/**
 * Finds one decoded field of an object by the schema, ordinal and occurrence the decoder recorded.
 * @return The value, or null when this body did not carry that field at all.
 */
[[nodiscard]] const sense::DecodedValue* sense_field(const sense::DecodedValue* values,
                                                     std::uint32_t count,
                                                     std::uint32_t schema,
                                                     std::uint16_t ordinal,
                                                     std::uint32_t occurrence = 0) noexcept {
    for (std::uint32_t index = 0; index < count; ++index) {
        if (values[index].schemaRow == schema && values[index].fieldOrdinal == ordinal
            && values[index].occurrence == occurrence) {
            return &values[index];
        }
    }
    return nullptr;
}

/** Room for one printed delta field, its sign and its terminator. */
using FieldText = std::array<char, 24>;

/**
 * Formats one field of a Sense delta. Sense reports only what changed, so a field this body did
 * not carry is unchanged, not zero, and prints as a dash.
 * @param value Decoded field, or null when the body did not carry it.
 * @param buffer Caller storage for the printed value.
 * @return Pointer into buffer, always terminated.
 */
[[nodiscard]] const char* sense_text(const sense::DecodedValue* value, FieldText& buffer) noexcept {
    buffer.fill('\0');
    if (value == nullptr || !value->present) {
        buffer[0] = '-';
        return buffer.data();
    }
    const long long printed = value->kind == sense::ValueKind::signedInteger
                                  ? static_cast<long long>(value->signedValue)
                                  : static_cast<long long>(value->unsignedValue);
    if (std::snprintf(buffer.data(), buffer.size(), "%lld", printed) <= 0) {
        buffer.fill('\0');
        buffer[0] = '?';
    }
    return buffer.data();
}

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

/**
 * Logs a body the decoder could not follow as hex, split across lines to fit the line cap.
 * @param stage Short stable stage name for the log line.
 * @param request Validated envelope.
 */
void report_payload(const char* stage, const message::Request& request) noexcept {
    if (!core::log::accepts(core::log::Channel::server, core::log::Level::debug)) {
        return;
    }
    // Two hex digits per byte, leaving room for the key prefix.
    constexpr std::size_t kChunkBytes = 448;
    const std::span<const std::byte> payload = request.payload;
    for (std::size_t offset = 0; offset < payload.size(); offset += kChunkBytes) {
        std::array<char, core::log::kLineCapacity> line{};
        const int written =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=activity stage=%s_raw type=%u bytes=%zu offset=%zu hex=",
                          stage,
                          request.messageType,
                          payload.size(),
                          offset);
        if (written <= 0 || static_cast<std::size_t>(written) >= line.size()) {
            return;
        }
        auto length = static_cast<std::size_t>(written);
        const std::size_t count = (std::min)(kChunkBytes, payload.size() - offset);
        static_cast<void>(core::log::append_hex(line, length, payload.subspan(offset, count)));
        core::log::write(
            core::log::Channel::server, core::log::Level::debug, {line.data(), length});
    }
}

} // namespace

/** Records the synchronous schema-selected sensor decode. */
Framed frame_sense_update(const message::Request& request,
                          const message::sense_update::SenseUpdate& update) noexcept {
    if (update.decoded.status == sense::DecodeStatus::malformed) {
        return {report_malformed("sense", request), update.decoded.bitsConsumed};
    }
    report(core::log::Level::debug,
           "ev=activity stage=sense result=%s epoch=0x%016llX%016llX roster_bits=%u groups=%u "
           "decoded_groups=%u skipped_groups=%u objects=%u decoded_objects=%u",
           sense::decode_status_name(update.decoded.status),
           static_cast<unsigned long long>(update.epoch.first),
           static_cast<unsigned long long>(update.epoch.second),
           update.rosterDeltaBits,
           update.decoded.groupsSeen,
           update.decoded.groupsDecoded,
           update.decoded.groupsSkipped,
           update.decoded.objectsSeen,
           update.decoded.objectsDecoded);
    if (update.decoded.status == sense::DecodeStatus::schemaUnavailable) {
        report_payload("sense", request);
    }
    for (std::size_t index = 0; index < update.decoded.objectCount; ++index) {
        const sense::DecodedObject& object = update.decoded.objects[index];
        report(core::log::Level::debug,
               "ev=activity stage=sense_object result=%s row=%zu key=0x%08X object=0x%08X "
               "object_row=%u slot_row=%u slot_type=%u slot_index=%u schema=0x%08X "
               "schema_row=%u generation=%u values=%u delta_bits=%u",
               sense::object_status_name(object.status),
               index,
               object.registryKey,
               object.objectTag,
               object.objectRow,
               object.slotRow,
               static_cast<unsigned>(object.slotType),
               static_cast<unsigned>(object.slotIndex),
               object.senseSchema,
               object.schemaRow,
               object.generationPlusOne,
               object.valueCount,
               object.deltaBits);
        if (object.status == sense::ObjectStatus::decoded && object.senseSchema == kSquadSenseSchema
            && object.firstValue <= update.decoded.valueCount) {
            const sense::DecodedValue* const own = update.decoded.values.data() + object.firstValue;
            const std::uint32_t count = object.valueCount;
            std::array<char, 64> members{};
            int offset = 0;
            for (std::uint32_t slot = 0; slot < kSquadSlotCapacity; ++slot) {
                const sense::DecodedValue* const per =
                    sense_field(own, count, kSquadCountsSchema, 0, slot);
                if (per == nullptr) {
                    break;
                }
                FieldText slotText{};
                const int wrote = std::snprintf(members.data() + offset,
                                                members.size() - static_cast<std::size_t>(offset),
                                                offset == 0 ? "%s" : ",%s",
                                                sense_text(per, slotText));
                if (wrote <= 0 || static_cast<std::size_t>(offset + wrote) >= members.size()) {
                    break;
                }
                offset += wrote;
            }
            FieldText spawnText{};
            FieldText aliveText{};
            FieldText stateText{};
            FieldText flag0Text{};
            FieldText flag3Text{};
            FieldText slotsText{};
            report(core::log::Level::debug,
                   "ev=activity stage=squad_sense result=decoded key=0x%08X index=%u "
                   "spawn_gen=%s alive=%s state=%s flag0=%s flag3=%s slots=%s members=%s",
                   object.registryKey,
                   static_cast<unsigned>(object.slotIndex),
                   sense_text(sense_field(own, count, kSquadSenseSchema, 0), spawnText),
                   sense_text(sense_field(own, count, kSquadSenseSchema, 3), aliveText),
                   sense_text(sense_field(own, count, kSquadSenseSchema, 7), stateText),
                   sense_text(sense_field(own, count, kSquadSenseSchema, 8), flag0Text),
                   sense_text(sense_field(own, count, kSquadSenseSchema, 9), flag3Text),
                   sense_text(sense_field(own, count, kSquadSlotsSchema, 0), slotsText),
                   members.data());
        }
        if (object.status == sense::ObjectStatus::decoded
            && object.senseSchema == kCombatantSenseSchema
            && object.firstValue <= update.decoded.valueCount) {
            const sense::DecodedValue* const own = update.decoded.values.data() + object.firstValue;
            const std::uint32_t count = object.valueCount;
            FieldText spawnText{};
            FieldText laneText{};
            FieldText generationText{};
            FieldText tickText{};
            FieldText busyText{};
            FieldText subscriptionText{};
            FieldText dependencyText{};
            FieldText detachedText{};
            FieldText snapshotText{};
            report(core::log::Level::debug,
                   "ev=activity stage=combatant_sense result=decoded key=0x%08X index=%u "
                   "spawn_rev=%s atom_lane=%s atom_gen=%s atom_tick=%s busy=%s "
                   "subscription=%s dependency=%s detached=%s snapshot=%s",
                   object.registryKey,
                   static_cast<unsigned>(object.slotIndex),
                   sense_text(sense_field(own, count, kCombatantSenseSchema, 0), spawnText),
                   sense_text(sense_field(own, count, kCombatantAtomsSchema, 0), laneText),
                   sense_text(sense_field(own, count, kCombatantAtomsSchema, 1), generationText),
                   sense_text(sense_field(own, count, kCombatantAtomsSchema, 2), tickText),
                   sense_text(sense_field(own, count, kCombatantAtomsSchema, 3), busyText),
                   sense_text(sense_field(own, count, kCombatantSenseSchema, 5), subscriptionText),
                   sense_text(sense_field(own, count, kCombatantSenseSchema, 6), dependencyText),
                   sense_text(sense_field(own, count, kCombatantSenseSchema, 10), detachedText),
                   sense_text(sense_field(own, count, kCombatantSenseSchema, 11), snapshotText));
        }
        if (object.status != sense::ObjectStatus::decoded
            || object.senseSchema != kObjectSenseSchema
            || object.valueCount != kObjectSenseValueCount
            || object.firstValue > update.decoded.valueCount
            || kObjectSenseValueCount > update.decoded.valueCount - object.firstValue) {
            continue;
        }
        const sense::DecodedValue* const values = update.decoded.values.data() + object.firstValue;
        report(core::log::Level::debug,
               "ev=activity stage=object_sense result=decoded key=0x%08X index=%u "
               "auth_gen=%lld live=%llu spawned=%llu entry=%lld mask0=0x%08llX "
               "mask1=0x%08llX replies=%llu",
               object.registryKey,
               static_cast<unsigned>(object.slotIndex),
               static_cast<long long>(values[0].signedValue),
               static_cast<unsigned long long>(values[1].unsignedValue),
               static_cast<unsigned long long>(values[2].unsignedValue),
               static_cast<long long>(values[3].signedValue),
               static_cast<unsigned long long>(values[4].unsignedValue),
               static_cast<unsigned long long>(values[5].unsignedValue),
               static_cast<unsigned long long>(values[6].unsignedValue));
    }
    const Verdict verdict =
        update.decoded.status == sense::DecodeStatus::complete ? Verdict::framed : Verdict::partial;
    return {verdict, update.decoded.bitsConsumed};
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
    return {parsed.tailBits == 0 ? Verdict::framed : Verdict::partial, consumed};
}

/** Frames a complete peer-reservation request without reserving a row. */
Framed frame_reservation_request(const message::Request& request) noexcept {
    telemetry::ReservationRequest parsed{};
    std::size_t consumed = 0;
    if (!telemetry::parse_reservation_request(request.payload, parsed, consumed)) {
        return {report_malformed("reservation", request), consumed};
    }
    report(core::log::Level::debug,
           "ev=activity stage=reservation result=read revision=%u records=%u",
           parsed.peerTableEpoch,
           static_cast<unsigned>(parsed.recordCount));
    return {Verdict::framed, consumed};
}

/** Frames a reservation release. */
Framed frame_reservation_release(const message::Request& request) noexcept {
    ledger::ReservationRelease release{};
    std::size_t consumed = 0;
    if (!ledger::parse_release(request.payload, release, consumed)) {
        return {report_malformed("reservation_release", request), consumed};
    }
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
           static_cast<unsigned long long>(leave.ownMemberKey));
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
           "ev=activity stage=connectivity result=read peer=0x%016llX reason=%d",
           static_cast<unsigned long long>(failure.peerKey),
           static_cast<int>(failure.failureReason));
    // The two bits are the last schema field; the rest of the ninth byte is padding.
    return {Verdict::framed, consumed};
}

/** Records a client heartbeat as a bounded body. */
Framed frame_heartbeat(const message::Request&) noexcept {
    // One runtime-selected nested definition, so the declared service length is the only bound.
    return {Verdict::partial, 0};
}

/** Frames a complete lag-switch report without applying network policy. */
Framed frame_lag_switch(const message::Request& request) noexcept {
    telemetry::LagSwitchReport parsed{};
    std::size_t consumed = 0;
    if (!telemetry::parse_lag_switch(request.payload, parsed, consumed)) {
        return {report_malformed("lag_switch", request), consumed};
    }
    report(core::log::Level::debug,
           "ev=activity stage=lag_switch result=read records=%u",
           static_cast<unsigned>(parsed.recordCount));
    return {Verdict::framed, consumed};
}

/** Frames a complete connection-quality report without applying network policy. */
Framed frame_connection_quality(const message::Request& request) noexcept {
    telemetry::ConnectionQualityReport parsed{};
    std::size_t consumed = 0;
    if (!telemetry::parse_connection_quality(request.payload, parsed, consumed)) {
        return {report_malformed("connection_quality", request), consumed};
    }
    report(core::log::Level::debug,
           "ev=activity stage=connection_quality result=read records=%u",
           static_cast<unsigned>(parsed.recordCount));
    return {Verdict::framed, consumed};
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
           "ev=activity stage=migration result=noted peer=0x%016llX bubble=%d",
           static_cast<unsigned long long>(proposal.memberKey),
           proposal.bubbleIndex);
    return {Verdict::framed, consumed};
}

/** Frames the fixed high-water telemetry block. */
Framed frame_high_water(const message::Request& request) noexcept {
    telemetry::HighWater block{};
    std::size_t consumed = 0;
    if (!telemetry::parse_high_water(request.payload, block, consumed)) {
        return {report_malformed("high_water", request), consumed};
    }
    return {Verdict::framed, consumed};
}

/** Frames one of the two opaque scalar messages. */
Framed frame_opaque_scalar(const message::Request& request) noexcept {
    std::int32_t value = 0;
    std::size_t consumed = 0;
    if (!telemetry::parse_opaque_scalar(request.payload, value, consumed)) {
        return {report_malformed("scalar", request), consumed};
    }
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

/** Frames one incident, optionally returning its parsed outer fields. */
static Framed frame_incident_impl(const message::Request& request,
                                  message::incident::Incident* output) noexcept {
    namespace incident = message::incident;
    incident::Incident parsed{};
    const incident::Verdict verdict = incident::validate(request.payload, parsed);
    const bool accepted = verdict == incident::Verdict::accepted;
    if (!accepted) {
        report(core::log::Level::warn,
               "ev=activity stage=incident result=%s target=%u extra=%u selector=%u "
               "optional=%u payload=%u",
               incident::verdict_name(verdict),
               parsed.primaryTarget,
               parsed.extraTargetCount,
               parsed.selectorLength,
               static_cast<unsigned>(parsed.hasOptionalBlock),
               parsed.payloadLength);
        // A refused target index would index the consumer's table unbounded, so the body is kept
        // and never relayed.
        const Verdict outcome = verdict == incident::Verdict::targetPoisoned
                                        || verdict == incident::Verdict::targetOutOfRange
                                    ? Verdict::quarantined
                                    : Verdict::malformed;
        return {outcome, parsed.consumedBits};
    }
    if (output != nullptr) {
        *output = parsed;
    }
    apply_incident_grants(parsed);
    apply_kill_progress(parsed);
    report(core::log::Level::debug,
           "ev=activity stage=incident result=accepted target=%u extra=%u selector=%u "
           "optional=%u payload=%u",
           parsed.primaryTarget,
           parsed.extraTargetCount,
           parsed.selectorLength,
           static_cast<unsigned>(parsed.hasOptionalBlock),
           parsed.payloadLength);
    return {Verdict::framed, parsed.consumedBits};
}

/** Frames one incident and quarantines a poison target. */
Framed frame_incident(const message::Request& request) noexcept {
    return frame_incident_impl(request, nullptr);
}

/** Frames one incident and returns its parsed outer fields. */
Framed frame_incident_copy(const message::Request& request,
                           message::incident::Incident& incident) noexcept {
    incident = {};
    return frame_incident_impl(request, &incident);
}

/** Frames one authority release, which records authority and returns no lease. */
Framed frame_authority_release(const message::Request& request, bool expectReason) noexcept {
    authority::Release decoded{};
    const bool parsed = expectReason ? authority::parse_abandon(request.payload, decoded)
                                     : authority::parse_abdicate(request.payload, decoded);
    if (!parsed) {
        return {report_malformed("authority", request), 0};
    }
    // The mask counts the lease the client believes it hands back with the bubble.
    std::size_t returning = 0;
    for (const std::byte byte : decoded.mask) {
        returning += static_cast<std::size_t>(std::popcount(std::to_integer<unsigned char>(byte)));
    }
    report(core::log::Level::debug,
           "ev=activity stage=authority result=noted type=%u selector=%u reason=%d slots=%zu",
           request.messageType,
           static_cast<unsigned>(decoded.selector),
           decoded.hasReason ? decoded.reason : 0,
           returning);
    for (std::size_t offset = 0; offset < decoded.mask.size(); offset += sizeof(std::uint32_t)) {
        const std::uint32_t word = middleware::encoding::read_u32_le(
            std::span(decoded.mask).subspan(offset).first<sizeof(std::uint32_t)>());
        if (word != 0) {
            report(core::log::Level::debug,
                   "ev=activity stage=authority_mask type=%u selector=%u word=%zu bits=0x%08X",
                   request.messageType,
                   static_cast<unsigned>(decoded.selector),
                   offset / sizeof(std::uint32_t),
                   word);
        }
    }
    return {Verdict::framed, payload_bits(request)};
}

/** Frames one purge request before its transactional message-25 answer. */
Framed frame_request_purge(const message::Request& request) noexcept {
    authority::PurgeRequest decoded{};
    if (!authority::parse_request_purge(request.payload, decoded)) {
        return {report_malformed("purge", request), 0};
    }
    report(core::log::Level::debug,
           "ev=activity stage=purge result=noted reason=%d mask_bytes=%zu",
           decoded.reason,
           decoded.mask.size());
    return {Verdict::framed, payload_bits(request)};
}

/** Frames one authority query answer. */
Framed frame_query_answer(const message::Request& request) noexcept {
    authority::QueryAnswer answer{};
    if (!authority::parse_query_answer(request.messageType, request.payload, answer)) {
        return {report_malformed("authority_answer", request), 0};
    }
    report(core::log::Level::debug,
           "ev=activity stage=authority result=answer type=%u corr=0x%08X selector=%d",
           request.messageType,
           static_cast<std::uint32_t>(answer.correlation),
           answer.hasSelector ? static_cast<int>(answer.selector) : -1);
    return {Verdict::framed, payload_bits(request)};
}

/** Records an envelope whose message type has no known body grammar. */
Framed frame_unknown(const message::Request& request) noexcept {
    report(core::log::Level::warn,
           "ev=activity stage=unknown result=bounded type=%u bytes=%zu",
           request.messageType,
           request.payload.size());
    return {Verdict::partial, 0};
}

} // namespace sunrise::server::bap::encrypted::activity_message::receipts
