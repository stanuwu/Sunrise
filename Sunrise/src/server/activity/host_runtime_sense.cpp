/**
 * Type-43 Sense retention, tracing and scalar selection.
 * Helpers here need the Host runtime lock; the entry points take it themselves.
 */

#include <algorithm>
#include <array>
#include <bit>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <new>
#include <span>

#include "../../core/logging/log.h"
#include "../../middleware/bap/activity_message/ghost_link_sense.h"
#include "../../middleware/bap/activity_message/sense_observation_packet.h"
#include "../../state/activity/runtime.h"
#include "../../state/activity_sdk/format.h"
#include "host_runtime_internal.h"

namespace sunrise::server::activity::host {
namespace detail {

/**
 * Tests whether every exposed object is complete inside one safely framed packet. A partial decode
 * must carry a partial receipt, and an unsafe or malformed sibling refuses the whole packet.
 */
bool usable_sense_observation_packet(
    const SenseEnvelope& envelope,
    const middleware::bap::activity_message::sense_update::DecodedPacket& packet) noexcept {
    const auto& [sourceGeneration,
                 clientMessageSequence,
                 verdict,
                 decodeStatus,
                 groupsSeen,
                 groupsDecoded,
                 groupsSkipped,
                 objectsSeen,
                 objectsDecoded] = envelope;
    namespace sense = middleware::bap::activity_message::sense_update;
    if (sourceGeneration == 0 || clientMessageSequence == 0
        || !((decodeStatus == sense::DecodeStatus::complete
              && verdict == state::activity::receipts::Verdict::framed)
             || (decodeStatus == sense::DecodeStatus::partial
                 && verdict == state::activity::receipts::Verdict::partial))
        || decodeStatus != packet.status || !sense::observation_packet(packet)
        || groupsSeen != packet.groupsSeen || groupsDecoded != packet.groupsDecoded
        || groupsSkipped != packet.groupsSkipped || objectsSeen != packet.objectsSeen
        || objectsDecoded != packet.objectsDecoded || packet.objectsDecoded == 0
        || packet.objectCount > packet.objectsSeen || packet.groupsDecoded > packet.groupsSeen
        || packet.groupsSkipped > packet.groupsSeen
        || packet.groupsDecoded != packet.groupsSeen - packet.groupsSkipped) {
        return false;
    }
    for (std::size_t index = 0; index < packet.objectCount; ++index) {
        const sense::DecodedObject& object = packet.objects[index];
        if (object.status == sense::ObjectStatus::unsafeCount) {
            return false;
        }
        if (object.status != sense::ObjectStatus::decoded) {
            continue;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            const sense::DecodedObject& other = packet.objects[prior];
            if (other.status == sense::ObjectStatus::decoded
                && other.registryKey == object.registryKey && other.objectTag == object.objectTag
                && other.senseSchema == object.senseSchema && other.schemaRow == object.schemaRow
                && other.slotIndex == object.slotIndex && other.slotType == object.slotType) {
                return false;
            }
        }
    }
    return true;
}

} // namespace detail

namespace {

using namespace detail;

/** @return True when one decoded object has the exact retained observation key. */
[[nodiscard]] bool same_sense_key(
    const SenseObservationKey& key,
    const middleware::bap::activity_message::sense_update::DecodedObject& object) noexcept {
    return key.registryKey == object.registryKey && key.objectTag == object.objectTag
           && key.slotType == object.slotType && key.slotIndex == object.slotIndex
           && key.senseSchema == object.senseSchema && key.schemaRow == object.schemaRow;
}

/** @return True when the packet carries this key at or after the selected object. */
[[nodiscard]] bool
packet_has_sense_key(const middleware::bap::activity_message::sense_update::DecodedPacket& packet,
                     const SenseObservationKey& key,
                     std::size_t first) noexcept {
    for (std::size_t index = first; index < packet.objectCount; ++index) {
        if (packet.objects[index].status
                == middleware::bap::activity_message::sense_update::ObjectStatus::decoded
            && packet.objects[index].hasGeneration && same_sense_key(key, packet.objects[index])) {
            return true;
        }
    }
    return false;
}

/** @return True when the envelope closed and each decoded object's storage is bounded. */
[[nodiscard]] bool valid_sense_observation_input(const SenseInput& input) noexcept {
    return detail::usable_sense_observation_packet(
        {.sourceGeneration = input.sourceGeneration,
         .clientMessageSequence = input.clientMessageSequence,
         .verdict = input.verdict,
         .decodeStatus = input.decodeStatus,
         .groupsSeen = input.groupsSeen,
         .groupsDecoded = input.groupsDecoded,
         .groupsSkipped = input.groupsSkipped,
         .objectsSeen = input.objectsSeen,
         .objectsDecoded = input.objectsDecoded},
        input.decoded);
}

/** Mixes one fixed-width value into the local scene change guard. */
void mix_scene_fingerprint(std::uint64_t& fingerprint, std::uint64_t value) noexcept {
    // FNV-1a 64-bit prime.
    constexpr std::uint64_t kPrime = 1'099'511'628'211ULL;
    for (std::uint8_t shift = 0; shift < 64; shift += 8) {
        fingerprint ^= (value >> shift) & 0xFFU;
        fingerprint *= kPrime;
    }
}

/** @return A run-local change guard over one decoded object and every retained value. */
[[nodiscard]] std::uint64_t
scene_fingerprint(const middleware::bap::activity_message::sense_update::DecodedObject& object,
                  std::span<const middleware::bap::activity_message::sense_update::DecodedValue>
                      values) noexcept {
    // FNV-1a 64-bit offset basis.
    constexpr std::uint64_t kOffset = 14'695'981'039'346'656'037ULL;
    std::uint64_t fingerprint = kOffset;
    mix_scene_fingerprint(fingerprint, object.objectRow);
    mix_scene_fingerprint(fingerprint, object.slotRow);
    mix_scene_fingerprint(fingerprint, object.schemaRow);
    mix_scene_fingerprint(fingerprint, object.generationPlusOne);
    mix_scene_fingerprint(fingerprint, object.deltaBits);
    mix_scene_fingerprint(fingerprint, object.hasGeneration ? 1U : 0U);
    mix_scene_fingerprint(fingerprint, values.size());
    for (const auto& value : values) {
        mix_scene_fingerprint(fingerprint, value.unsignedValue);
        mix_scene_fingerprint(fingerprint, static_cast<std::uint64_t>(value.signedValue));
        mix_scene_fingerprint(fingerprint, std::bit_cast<std::uint32_t>(value.realValue));
        mix_scene_fingerprint(fingerprint, value.schemaRow);
        mix_scene_fingerprint(fingerprint, value.fieldRow);
        mix_scene_fingerprint(fingerprint, value.occurrence);
        mix_scene_fingerprint(fingerprint, value.bitOffset);
        mix_scene_fingerprint(fingerprint, value.fieldOrdinal);
        mix_scene_fingerprint(fingerprint, value.width);
        mix_scene_fingerprint(fingerprint, static_cast<std::uint8_t>(value.kind));
        mix_scene_fingerprint(fingerprint, value.present ? 1U : 0U);
    }
    return fingerprint;
}

/** Writes one bounded type-43 diagnostic event. */
void report_scene_sense(const char* format, ...) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(line.data(), line.size(), format, arguments);
    va_end(arguments);
    if (written <= 0) {
        return;
    }
    const auto length = static_cast<std::size_t>(written) < line.size()
                            ? static_cast<std::size_t>(written)
                            : line.size() - 1;
    core::log::write(core::log::Channel::server, core::log::Level::debug, {line.data(), length});
}

/** @return The trace row for one exact ClientRef, or null when it has not been seen. */
[[nodiscard]] SceneSenseTraceRecord* find_scene_trace_record(
    SceneSenseTrace& trace,
    const middleware::bap::activity_message::sense_update::DecodedObject& object) noexcept {
    for (SceneSenseTraceRecord& record : trace.records) {
        if (record.occupied && same_sense_key(record.key, object)) {
            return &record;
        }
    }
    return nullptr;
}

/** @return One unused trace row, or null when the bounded table is full. */
[[nodiscard]] SceneSenseTraceRecord* reserve_scene_trace_record(SceneSenseTrace& trace) noexcept {
    for (SceneSenseTraceRecord& record : trace.records) {
        if (!record.occupied) {
            return &record;
        }
    }
    return nullptr;
}

/** Reports one typed scalar with its exact reflected rows and wire position. */
void report_scene_value(
    const Instance& instance,
    const SenseInput& input,
    const middleware::bap::activity_message::sense_update::DecodedObject& object,
    const middleware::bap::activity_message::sense_update::DecodedValue& value,
    std::size_t valueIndex) noexcept {
    using ValueKind = middleware::bap::activity_message::sense_update::ValueKind;
    // Fixed head of the scene-sense value line; the typed value is appended after it.
    constexpr const char* kPrefix =
        "ev=scene_sense kind=value activity=%d session=0x%llX binding_rev=%llu "
        "source_gen=%llu msg_seq=%llu key=0x%08X tag=0x%08X type=%u index=%u "
        "sense_schema=0x%08X gen_plus_one=%u value_index=%zu schema_row=%u "
        "field_row=%u ordinal=%u occurrence=%u bit=%u width=%u present=%u";
    std::array<char, core::log::kLineCapacity> prefix{};
    const int written =
        std::snprintf(prefix.data(),
                      prefix.size(),
                      kPrefix,
                      static_cast<int>(instance.view.binding.destination.activityIndex),
                      static_cast<unsigned long long>(instance.view.binding.sessionId),
                      static_cast<unsigned long long>(instance.view.binding.createdRevision),
                      static_cast<unsigned long long>(input.sourceGeneration),
                      static_cast<unsigned long long>(input.clientMessageSequence),
                      object.registryKey,
                      object.objectTag,
                      static_cast<unsigned>(object.slotType),
                      static_cast<unsigned>(object.slotIndex),
                      object.senseSchema,
                      object.generationPlusOne,
                      valueIndex,
                      value.schemaRow,
                      value.fieldRow,
                      static_cast<unsigned>(value.fieldOrdinal),
                      value.occurrence,
                      value.bitOffset,
                      static_cast<unsigned>(value.width),
                      value.present ? 1U : 0U);
    if (written <= 0 || static_cast<std::size_t>(written) >= prefix.size()) {
        return;
    }
    if (!value.present) {
        report_scene_sense("%s domain=%s value=absent",
                           prefix.data(),
                           value.kind == ValueKind::unsignedInteger ? "uint"
                           : value.kind == ValueKind::signedInteger ? "int"
                           : value.kind == ValueKind::boolean       ? "bool"
                                                                    : "real32");
        return;
    }
    switch (value.kind) {
    case ValueKind::unsignedInteger:
        report_scene_sense("%s domain=uint value=0x%llX",
                           prefix.data(),
                           static_cast<unsigned long long>(value.unsignedValue));
        break;
    case ValueKind::signedInteger:
        report_scene_sense("%s domain=int raw=0x%llX value=%lld",
                           prefix.data(),
                           static_cast<unsigned long long>(value.unsignedValue),
                           static_cast<long long>(value.signedValue));
        break;
    case ValueKind::boolean:
        report_scene_sense("%s domain=bool raw=0x%llX value=%u",
                           prefix.data(),
                           static_cast<unsigned long long>(value.unsignedValue),
                           value.unsignedValue != 0 ? 1U : 0U);
        break;
    case ValueKind::real32:
        report_scene_sense("%s domain=real32 raw=0x%llX value_bits=0x%08X value=%.9g",
                           prefix.data(),
                           static_cast<unsigned long long>(value.unsignedValue),
                           std::bit_cast<std::uint32_t>(value.realValue),
                           static_cast<double>(value.realValue));
        break;
    }
}

/** Reports complete changed type-43 objects without changing retained activity state. */
void trace_scene_sense(Instance& instance, const SenseInput& input) noexcept {
    namespace sense = middleware::bap::activity_message::sense_update;
    if (!core::log::accepts(core::log::Channel::server, core::log::Level::debug)) {
        return;
    }
    SceneSenseTrace& trace = instance.sceneSenseTrace;
    if (trace.sourceGeneration != input.sourceGeneration) {
        trace = {};
        trace.sourceGeneration = input.sourceGeneration;
    }
    const sense::DecodedPacket& packet = input.decoded;
    bool hasScene = false;
    const std::size_t retainedObjectCount = (std::min)(packet.objectCount, packet.objects.size());
    for (std::size_t index = 0; index < retainedObjectCount; ++index) {
        if (packet.objects[index].slotType
            == static_cast<std::uint8_t>(state::activity_sdk::format::kAuthoredSceneSlotType)) {
            hasScene = true;
            break;
        }
    }
    if (!valid_sense_observation_input(input)) {
        if (!trace.incompleteReported && (hasScene || packet.objectsTruncated)) {
            trace.incompleteReported = true;
            report_scene_sense(
                "ev=scene_sense kind=packet result=skip reason=incomplete activity=%d "
                "session=0x%llX binding_rev=%llu source_gen=%llu msg_seq=%llu "
                "status=%s objects_seen=%u objects_kept=%zu values_kept=%zu "
                "objects_truncated=%u values_truncated=%u",
                static_cast<int>(instance.view.binding.destination.activityIndex),
                static_cast<unsigned long long>(instance.view.binding.sessionId),
                static_cast<unsigned long long>(instance.view.binding.createdRevision),
                static_cast<unsigned long long>(input.sourceGeneration),
                static_cast<unsigned long long>(input.clientMessageSequence),
                sense::decode_status_name(packet.status),
                packet.objectsSeen,
                packet.objectCount,
                packet.valueCount,
                packet.objectsTruncated ? 1U : 0U,
                packet.valuesTruncated ? 1U : 0U);
        }
        return;
    }
    for (std::size_t index = 0; index < packet.objectCount; ++index) {
        const sense::DecodedObject& object = packet.objects[index];
        if (object.status != sense::ObjectStatus::decoded || !object.hasGeneration
            || object.slotType
                   != static_cast<std::uint8_t>(state::activity_sdk::format::kAuthoredSceneSlotType)
            || packet_has_sense_key(packet,
                                    {object.registryKey,
                                     object.objectTag,
                                     object.senseSchema,
                                     object.schemaRow,
                                     object.slotIndex,
                                     object.slotType},
                                    index + 1)) {
            continue;
        }
        const std::span values(packet.values.data() + object.firstValue, object.valueCount);
        const std::uint64_t fingerprint = scene_fingerprint(object, values);
        SceneSenseTraceRecord* record = find_scene_trace_record(trace, object);
        const bool known = record != nullptr;
        if (known && record->fingerprint == fingerprint
            && record->generationPlusOne == object.generationPlusOne
            && record->valueCount == object.valueCount
            && record->hasGeneration == object.hasGeneration) {
            continue;
        }
        if (record == nullptr) {
            record = reserve_scene_trace_record(trace);
        }
        if (record == nullptr) {
            if (!trace.capacityReported) {
                trace.capacityReported = true;
                report_scene_sense(
                    "ev=scene_sense kind=packet result=skip reason=trace_capacity activity=%d "
                    "session=0x%llX binding_rev=%llu source_gen=%llu capacity=%zu",
                    static_cast<int>(instance.view.binding.destination.activityIndex),
                    static_cast<unsigned long long>(instance.view.binding.sessionId),
                    static_cast<unsigned long long>(instance.view.binding.createdRevision),
                    static_cast<unsigned long long>(input.sourceGeneration),
                    trace.records.size());
            }
            continue;
        }
        record->key = {object.registryKey,
                       object.objectTag,
                       object.senseSchema,
                       object.schemaRow,
                       object.slotIndex,
                       object.slotType};
        record->fingerprint = fingerprint;
        record->generationPlusOne = object.generationPlusOne;
        record->valueCount = object.valueCount;
        record->hasGeneration = object.hasGeneration;
        record->occupied = true;
        report_scene_sense("ev=scene_sense kind=object change=%s activity=%d session=0x%llX "
                           "binding_rev=%llu source_gen=%llu msg_seq=%llu key=0x%08X tag=0x%08X "
                           "object_row=%u type=%u index=%u slot_row=%u sense_schema=0x%08X "
                           "schema_row=%u gen_plus_one=%u has_gen=%u delta_bits=%u values=%u",
                           known ? "update" : "new",
                           static_cast<int>(instance.view.binding.destination.activityIndex),
                           static_cast<unsigned long long>(instance.view.binding.sessionId),
                           static_cast<unsigned long long>(instance.view.binding.createdRevision),
                           static_cast<unsigned long long>(input.sourceGeneration),
                           static_cast<unsigned long long>(input.clientMessageSequence),
                           object.registryKey,
                           object.objectTag,
                           object.objectRow,
                           static_cast<unsigned>(object.slotType),
                           static_cast<unsigned>(object.slotIndex),
                           object.slotRow,
                           object.senseSchema,
                           object.schemaRow,
                           object.generationPlusOne,
                           object.hasGeneration ? 1U : 0U,
                           object.deltaBits,
                           object.valueCount);
        for (std::size_t valueIndex = 0; valueIndex < values.size(); ++valueIndex) {
            report_scene_value(instance, input, object, values[valueIndex], valueIndex);
        }
    }
}

/** Replaces only fully decoded keys in an accepted packet; omitted or unsupported keys stay
 * retained. */
[[nodiscard]] bool
retain_sense_observations(Instance& instance, const SenseInput& input, std::uint64_t now) noexcept {
    namespace sense = middleware::bap::activity_message::sense_update;
    if (!valid_sense_observation_input(input)) {
        return false;
    }
    const sense::DecodedPacket& packet = input.decoded;
    SenseObservationSnapshot next{};
    next.revision = next_nonzero(instance.senseObservations.revision);
    next.sourceGeneration = input.sourceGeneration;
    for (std::size_t index = 0; index < packet.objectCount; ++index) {
        const sense::DecodedObject& object = packet.objects[index];
        if (object.status != sense::ObjectStatus::decoded || !object.hasGeneration) {
            continue;
        }
        const SenseObservationKey key{object.registryKey,
                                      object.objectTag,
                                      object.senseSchema,
                                      object.schemaRow,
                                      object.slotIndex,
                                      object.slotType};
        if (packet_has_sense_key(packet, key, index + 1)) {
            continue;
        }
        SenseObservation observation{};
        observation.binding = input.binding;
        observation.key = key;
        observation.sequence = next.revision;
        observation.tick = now;
        observation.sourceGeneration = input.sourceGeneration;
        observation.clientMessageSequence = input.clientMessageSequence;
        observation.generationPlusOne = object.generationPlusOne;
        observation.hasGeneration = object.hasGeneration;
        const std::span values(packet.values.data() + object.firstValue, object.valueCount);
        if (!append_sense_observation(next, observation, values)) {
            return false;
        }
    }
    if (instance.senseObservations.sourceGeneration == input.sourceGeneration) {
        const SenseObservationSnapshot& current = instance.senseObservations;
        for (std::size_t index = 0; index < current.observationCount; ++index) {
            const SenseObservation& observation = current.observations[index];
            if (packet_has_sense_key(packet, observation.key, 0)
                || observation.firstValue > current.valueCount
                || observation.valueCount > current.valueCount - observation.firstValue) {
                continue;
            }
            const std::span values(current.values.data() + observation.firstValue,
                                   observation.valueCount);
            static_cast<void>(append_sense_observation(next, observation, values));
        }
    }
    instance.senseObservations = next;
    instance.view.senseObservationCount = static_cast<std::uint32_t>(next.observationCount);
    instance.view.senseObservationValueCount = static_cast<std::uint32_t>(next.valueCount);
    instance.view.senseObservationRevision = next.revision;
    instance.view.senseObservationSourceGeneration = next.sourceGeneration;
    return true;
}

/** Merges complete squad deltas without discarding fields absent from a later report. */
void retain_squad_sense(Instance& instance, const SenseInput& input) noexcept {
    namespace sense = middleware::bap::activity_message::sense_update;
    namespace squadSense = middleware::bap::activity_message::squad_sense;
    const sense::DecodedPacket& packet = input.decoded;
    if (input.sourceGeneration == 0 || input.sourceGeneration < instance.squadSenseSourceGeneration
        || packet.status == sense::DecodeStatus::malformed || packet.valuesTruncated
        || packet.objectsTruncated || packet.objectCount > packet.objects.size()
        || packet.valueCount > packet.values.size()) {
        return;
    }
    if (input.sourceGeneration != instance.squadSenseSourceGeneration) {
        instance.squadSense.clear();
        instance.squadSenseSourceGeneration = input.sourceGeneration;
    }
    for (const sense::DecodedObject& object : std::span(packet.objects).first(packet.objectCount)) {
        if (object.slotType != squadSense::kSlotType || object.senseSchema != squadSense::kSchema
            || object.status != sense::ObjectStatus::decoded || !object.hasGeneration) {
            continue;
        }
        auto found = std::ranges::find_if(instance.squadSense, [&](const SquadSenseRecord& record) {
            return same_sense_key(record.key, object);
        });
        squadSense::State merged =
            found == instance.squadSense.end() ? squadSense::State{} : found->state;
        // An uninitialized replica cannot replace the squad's recovery state.
        if (!squadSense::merge(merged, object, std::span(packet.values).first(packet.valueCount))
            || !merged.valid) {
            continue;
        }
        if (found != instance.squadSense.end()) {
            found->state = merged;
        } else if (instance.squadSense.size() < kScriptableGuardCapacity) {
            const SenseObservationKey key{object.registryKey,
                                          object.objectTag,
                                          object.senseSchema,
                                          object.schemaRow,
                                          object.slotIndex,
                                          object.slotType};
            try {
                instance.squadSense.push_back({key, merged});
            } catch (const std::bad_alloc&) {
                return;
            }
        }
    }
}

/** @return The committed guard for one exact reported slot, or null when none was armed. */
[[nodiscard]] ScriptableGuard* find_ghost_link_guard(
    Instance& instance,
    const middleware::bap::activity_message::sense_update::DecodedObject& object) noexcept {
    for (ScriptableGuard& guard : instance.scriptableGuards) {
        if (guard.occupied && guard.target.registryKey == object.registryKey
            && guard.target.objectTag == object.objectTag
            && guard.target.slotType == object.slotType
            && guard.target.slotIndex == object.slotIndex) {
            return &guard;
        }
    }
    return nullptr;
}

/**
 * Merges the client's type-65 reports and latches the end of the bar.
 * The client cannot cross the duration on its own, so reaching the end is a host-owned change: it
 * advances the state revision and the next roster carries the finishing Sense root.
 */
void retain_ghost_link_sense(Instance& instance, const SenseInput& input) noexcept {
    namespace sense = middleware::bap::activity_message::sense_update;
    namespace ghostAuth = middleware::bap::activity_message::ghost_link;
    namespace ghostSense = middleware::bap::activity_message::ghost_link_sense;
    const sense::DecodedPacket& packet = input.decoded;
    if (input.sourceGeneration == 0 || packet.objectCount > packet.objects.size()
        || packet.valueCount > packet.values.size()) {
        return;
    }
    for (const sense::DecodedObject& object : std::span(packet.objects).first(packet.objectCount)) {
        if (object.slotType != ghostAuth::kSlotType || object.senseSchema != ghostAuth::kSenseSchema
            || object.status != sense::ObjectStatus::decoded || !object.hasGeneration
            || object.firstValue > packet.valueCount
            || object.valueCount > packet.valueCount - object.firstValue) {
            continue;
        }
        ScriptableGuard* const guard = find_ghost_link_guard(instance, object);
        ghostSense::Level level{};
        if (guard == nullptr
            || !ghostSense::read(
                std::span(packet.values).subspan(object.firstValue, object.valueCount), level)) {
            continue;
        }
        GhostLinkScan& scan = guard->ghostLink;
        scan.counter = object.generationPlusOne;
        scan.counterKnown = true;
        scan.fraction = level.fraction;
        scan.active = level.active;
        const bool ended = scan.armed && !scan.finished && scan.generation != 0
                           && level.generation == static_cast<std::int32_t>(scan.generation)
                           && level.active && level.fraction >= kGhostLinkFinishFraction;
        if (ended && instance.view.stateRevision != (std::numeric_limits<std::uint64_t>::max)()) {
            scan.finished = true;
            ++instance.view.stateRevision;
        }
    }
}

} // namespace

namespace detail {

/** Appends one observation and its complete owned value range. */
[[nodiscard]] bool append_sense_observation(
    SenseObservationSnapshot& output,
    SenseObservation observation,
    std::span<const middleware::bap::activity_message::sense_update::DecodedValue>
        values) noexcept {
    if (output.observationCount == output.observations.size()
        || values.size() > output.values.size() - output.valueCount) {
        return false;
    }
    observation.firstValue = static_cast<std::uint32_t>(output.valueCount);
    observation.valueCount = static_cast<std::uint32_t>(values.size());
    output.observations[output.observationCount++] = observation;
    std::copy(values.begin(),
              values.end(),
              output.values.begin() + static_cast<std::ptrdiff_t>(output.valueCount));
    output.valueCount += values.size();
    return true;
}

/** Applies one copied msg-6 decode summary. */
void apply_sense(const SenseInput& input, std::uint64_t now) noexcept {
    Instance* const instance = find_instance(input.binding);
    if (instance == nullptr || !instance->view.active) {
        ++g_droppedIngress;
        return;
    }
    touch(*instance);
    ++instance->view.senseCount;
    trace_scene_sense(*instance, input);
    retain_squad_sense(*instance, input);
    retain_ghost_link_sense(*instance, input);
    static_cast<void>(retain_sense_observations(*instance, input, now));
    Event event{};
    event.attemptGeneration = input.attemptGeneration;
    event.binding = input.binding;
    event.tick = now;
    event.kind = EventKind::senseUpdate;
    event.epochFirst = input.epochFirst;
    event.epochSecond = input.epochSecond;
    event.payloadBytes = input.payloadBytes;
    event.peerHeardMask = input.peerHeardMask;
    event.tailBits = input.tailBits;
    event.consumedBits = input.consumedBits;
    event.firstGroupBits = input.firstGroupBits;
    event.firstRegistryKey = input.firstRegistryKey;
    event.groupsSeen = input.groupsSeen;
    event.groupsDecoded = input.groupsDecoded;
    event.groupsSkipped = input.groupsSkipped;
    event.objectsSeen = input.objectsSeen;
    event.objectsDecoded = input.objectsDecoded;
    event.firstSlotIndex = input.firstSlotIndex;
    event.firstSlotType = input.firstSlotType;
    // The event names one slot only: the first decoded ClientRef of the packet.
    if (input.decoded.objectCount != 0) {
        event.slotObjectTag = input.decoded.objects.front().objectTag;
        event.slotSenseSchema = input.decoded.objects.front().senseSchema;
    }
    event.senseDecodeStatus = input.decodeStatus;
    event.senseSnapshotRetained = valid_sense_observation_input(input);
    event.hasFirstObject = input.hasFirstObject;
    event.stateRevision = instance->view.stateRevision;
    event.sourceGeneration = input.sourceGeneration;
    event.clientMessageSequence = input.clientMessageSequence;
    event.lifetimeState = instance->view.lifetimeState;
    event.verdict = input.verdict;
    append_event(event);
    append_mission_input(event, event.senseSnapshotRetained ? &input.decoded : nullptr);
    instance->view.lastEventSequence = g_sequence;
}

} // namespace detail

/** Queues one owned msg-6 prefix for the Activity Host service. */
IngressRefusal submit_sense(const SenseInput& input) noexcept {
    if (!state::activity::binding_matches(input.binding)) {
        return IngressRefusal::binding;
    }
    if (input.sourceGeneration == 0) {
        return IngressRefusal::generation;
    }
    AcquireSRWLockExclusive(&g_lock);
    PendingInput pending{};
    pending.kind = PendingKind::sense;
    pending.sense = input;
    if (!append_pending(pending)) {
        ++g_droppedIngress;
        ReleaseSRWLockExclusive(&g_lock);
        return IngressRefusal::queue;
    }
    ++g_queuedIngress;
    ReleaseSRWLockExclusive(&g_lock);
    return IngressRefusal::none;
}

/** Copies initialized recovery state for one exact ActivityClient generation. */
bool snapshot_squad_sense(const state::activity::SessionBinding& binding,
                          std::uint64_t sourceGeneration,
                          const SenseObservationKey& key,
                          middleware::bap::activity_message::squad_sense::State& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const Instance* const instance = find_instance(binding);
    bool found = false;
    if (instance != nullptr && instance->squadSenseSourceGeneration == sourceGeneration) {
        for (const SquadSenseRecord& record : instance->squadSense) {
            if (record.key.registryKey == key.registryKey && record.key.objectTag == key.objectTag
                && record.key.senseSchema == key.senseSchema && record.key.slotType == key.slotType
                && record.key.slotIndex == key.slotIndex && record.state.valid) {
                output = record.state;
                found = true;
                break;
            }
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

/** Copies the armed scan model for one exact type-65 slot that has reported at least once. */
bool ghost_link_scan(const state::activity::SessionBinding& binding,
                     const SenseObservationKey& key,
                     GhostLinkLevel& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const Instance* const instance = find_instance(binding);
    bool found = false;
    if (instance != nullptr) {
        for (const ScriptableGuard& guard : instance->scriptableGuards) {
            const GhostLinkScan& scan = guard.ghostLink;
            if (!guard.occupied || guard.target.registryKey != key.registryKey
                || guard.target.objectTag != key.objectTag || guard.target.slotType != key.slotType
                || guard.target.slotIndex != key.slotIndex || !scan.armed || scan.generation == 0
                || !scan.counterKnown) {
                continue;
            }
            output.generation = static_cast<std::int32_t>(scan.generation);
            output.counter = scan.counter;
            output.finished = scan.finished;
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

/** Copies the latest complete Sense observations for one exact activity generation. */
bool snapshot_sense_observations(const state::activity::SessionBinding& binding,
                                 SenseObservationSnapshot& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const Instance* const instance = find_instance(binding);
    const bool found = instance != nullptr;
    if (found) {
        output = instance->senseObservations;
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

/** Selects one exact reflected scalar without changing the retained snapshot. */
SenseScalarStatus select_sense_scalar(const SenseObservationSnapshot& snapshot,
                                      const state::activity::SessionBinding& binding,
                                      const SenseScalarIdentity& identity,
                                      SenseScalarSample& output) noexcept {
    namespace sense = middleware::bap::activity_message::sense_update;
    output = {};
    if (binding.sessionId == state::activity::kAbsentSessionId
        || binding.createdRevision == state::activity::kInvalidRevision
        || identity.object.schemaRow == sense::kAbsentRuntimeRow
        || identity.fieldSchemaRow == sense::kAbsentRuntimeRow
        || identity.fieldRow == sense::kAbsentRuntimeRow
        || identity.fieldSchemaRow != identity.object.schemaRow) {
        return SenseScalarStatus::invalidIdentity;
    }
    if (snapshot.sourceGeneration == 0 || snapshot.observationCount > snapshot.observations.size()
        || snapshot.valueCount > snapshot.values.size()) {
        return SenseScalarStatus::invalidSnapshot;
    }
    const auto sameKey = [&identity](const SenseObservationKey& candidate) noexcept {
        const SenseObservationKey& expected = identity.object;
        return candidate.registryKey == expected.registryKey
               && candidate.objectTag == expected.objectTag
               && candidate.senseSchema == expected.senseSchema
               && candidate.schemaRow == expected.schemaRow
               && candidate.slotIndex == expected.slotIndex
               && candidate.slotType == expected.slotType;
    };
    bool found = false;
    for (std::size_t observationIndex = 0; observationIndex < snapshot.observationCount;
         ++observationIndex) {
        const SenseObservation& observation = snapshot.observations[observationIndex];
        if (!same_binding(observation.binding, binding) || !sameKey(observation.key)) {
            continue;
        }
        if (observation.sourceGeneration != snapshot.sourceGeneration
            || observation.firstValue > snapshot.valueCount
            || observation.valueCount > snapshot.valueCount - observation.firstValue) {
            output = {};
            return SenseScalarStatus::invalidSnapshot;
        }
        for (std::size_t valueIndex = 0; valueIndex < observation.valueCount; ++valueIndex) {
            const sense::DecodedValue& value = snapshot.values[observation.firstValue + valueIndex];
            if (value.schemaRow != identity.fieldSchemaRow || value.fieldRow != identity.fieldRow
                || value.fieldOrdinal != identity.fieldOrdinal
                || value.occurrence != identity.occurrence || value.kind != identity.kind) {
                continue;
            }
            if (found) {
                output = {};
                return SenseScalarStatus::ambiguous;
            }
            output.binding = observation.binding;
            output.identity = identity;
            output.value = value;
            output.observationRevision = observation.sequence;
            output.tick = observation.tick;
            output.sourceGeneration = observation.sourceGeneration;
            output.clientMessageSequence = observation.clientMessageSequence;
            output.generationPlusOne = observation.generationPlusOne;
            output.hasGeneration = observation.hasGeneration;
            found = true;
        }
    }
    return found ? SenseScalarStatus::ready : SenseScalarStatus::notFound;
}

/** @return True when two selected scalars have the same presence and raw typed value. */
bool same_sense_scalar_value(const SenseScalarSample& left,
                             const SenseScalarSample& right) noexcept {
    if (left.value.kind != right.value.kind || left.value.present != right.value.present) {
        return false;
    }
    if (!left.value.present) {
        return true;
    }
    return left.value.unsignedValue == right.value.unsignedValue
           && left.value.signedValue == right.value.signedValue
           && std::bit_cast<std::uint32_t>(left.value.realValue)
                  == std::bit_cast<std::uint32_t>(right.value.realValue);
}

/** @return True when both samples name the same ActivityClient and reported object generations. */
bool same_sense_scalar_generations(const SenseScalarSample& left,
                                   const SenseScalarSample& right) noexcept {
    return left.sourceGeneration != 0 && left.sourceGeneration == right.sourceGeneration
           && left.hasGeneration && right.hasGeneration
           && left.generationPlusOne == right.generationPlusOne;
}

/** @return Stable text for one exact scalar-selection result. */
const char* sense_scalar_status_name(SenseScalarStatus status) noexcept {
    switch (status) {
    case SenseScalarStatus::ready:
        return "ready";
    case SenseScalarStatus::invalidIdentity:
        return "invalid_identity";
    case SenseScalarStatus::invalidSnapshot:
        return "invalid_snapshot";
    case SenseScalarStatus::notFound:
        return "not_found";
    case SenseScalarStatus::ambiguous:
        return "ambiguous";
    }
    return "invalid_status";
}

} // namespace sunrise::server::activity::host
