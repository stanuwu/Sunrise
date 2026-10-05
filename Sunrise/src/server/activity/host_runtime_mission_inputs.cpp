/**
 * The ordered accepted-input feed the mission programs read.
 * Helpers here need the Host runtime lock; the entry points take it themselves.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#include <vector>

#include "../../core/logging/log.h"
#include "../../middleware/bap/activity_message/sense_observation_packet.h"
#include "../../state/activity/mission/runtime.h"
#include "../../state/activity/runtime.h"
#include "host_runtime_internal.h"

namespace sunrise::server::activity::host {
namespace detail {

/** The feed is unbounded; it holds every accepted row until its program commits it. */
std::vector<MissionInputRecord> g_missionInputs{};

} // namespace detail

namespace {

using namespace detail;

std::uint64_t g_missionInputSequence{};

/** @return True when this event kind enters the ordered mission-input feed. */
[[nodiscard]] bool mission_input_kind(EventKind kind) noexcept {
    return kind == EventKind::senseUpdate || kind == EventKind::incidentReceived
           || kind == EventKind::clientStateChanged || kind == EventKind::entitySlotsRequested
           || kind == EventKind::clientMessageReceived;
}

/**
 * @return True when the feed reserved room for one more accepted row. The feed holds every
 * accepted row until its program commits it, so it has no row count of its own. Only the
 * allocator can refuse.
 */
[[nodiscard]] bool reserve_mission_input_slot() noexcept {
    if (g_missionInputs.size() < g_missionInputs.capacity()) {
        return true;
    }
    // Grow a page at a time so the append that spends a durable sequence cannot reallocate.
    try {
        g_missionInputs.reserve(g_missionInputs.capacity() + kMissionInputReadPageSize);
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

/** Reports one client input the mission-input feed could not store. */
void report_mission_input_refusal(const Event& event) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=mission_input result=allocation_refused session=0x%llX binding_rev=%llu "
                      "kind=%u retained=%zu",
                      static_cast<unsigned long long>(event.binding.sessionId),
                      static_cast<unsigned long long>(event.binding.createdRevision),
                      static_cast<unsigned>(event.kind),
                      g_missionInputs.size());
    if (written > 0) {
        core::log::write(
            core::log::Channel::server,
            core::log::Level::debug,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1)});
    }
}

/** @return True when durable mission State still owes this retained accepted row. */
[[nodiscard]] bool mission_input_owed(const MissionInputRecord& record) noexcept {
    state::activity::mission::InputSequenceSnapshot cursors{};
    // A faulted binding owes nothing. Its program is skipped, so it never commits, and its rows
    // would otherwise be retained for the life of the process.
    if (!state::activity::mission::input_sequence_snapshot(record.view.event.binding, cursors)
        || cursors.faulted) {
        return false;
    }
    const std::uint64_t sequence = record.view.event.missionSequence;
    return sequence > cursors.committed && sequence <= cursors.issued;
}

/** Drops every retained row that durable mission State no longer owes. */
void retire_settled_mission_inputs() noexcept {
    static_cast<void>(std::erase_if(g_missionInputs, [](const MissionInputRecord& record) noexcept {
        return !mission_input_owed(record);
    }));
}

/** @return The retained row holding one exact feed sequence, or null when it is gone. */
[[nodiscard]] const MissionInputRecord* find_mission_input(std::uint64_t sequence) noexcept {
    for (std::size_t offset = g_missionInputs.size(); offset != 0; --offset) {
        const MissionInputRecord& candidate = g_missionInputs[offset - 1];
        if (candidate.view.sequence == sequence) {
            return &candidate;
        }
    }
    return nullptr;
}

} // namespace

namespace detail {

/** Assigns one exact binding's ordered client mission-input sequence. */
void stamp_mission_sequence(Event& event) noexcept {
    if (!mission_input_kind(event.kind)) {
        return;
    }
    Instance* const instance = find_instance(event.binding);
    if (instance == nullptr) {
        return;
    }
    // The durable sequence is what makes a row owed, so refuse before spending it.
    if (!reserve_mission_input_slot()) {
        report_mission_input_refusal(event);
        return;
    }
    std::uint64_t sequence = 0;
    std::uint64_t attemptGeneration = 0;
    if (state::activity::mission::issue_input_sequence(
            event.binding, sequence, &attemptGeneration)) {
        instance->missionSequence = sequence;
        event.missionSequence = sequence;
        if (event.attemptGeneration == 0) {
            event.attemptGeneration = attemptGeneration;
        }
    }
}

/** Retains one accepted client input independently from panel and output events. */
void append_mission_input(
    const Event& event,
    const middleware::bap::activity_message::sense_update::DecodedPacket* sense,
    const ClientMessageSnapshot* clientMessage) noexcept {
    if (event.missionSequence == 0) {
        return;
    }
    g_missionInputSequence = next_nonzero(g_missionInputSequence);
    MissionInputRecord record{};
    record.view.event = event;
    record.view.sequence = g_missionInputSequence;
    if (record.view.event.sequence == 0) {
        record.view.event.sequence = g_missionInputSequence;
    }
    if (sense != nullptr) {
        record.sense = *sense;
        record.hasSense = true;
    }
    if (clientMessage != nullptr) {
        record.clientMessage = *clientMessage;
        record.hasClientMessage = true;
    }
    // The slot was reserved when the durable sequence was issued, so this never reallocates.
    g_missionInputs.push_back(record);
}

/** Drops every retained accepted row and restarts the feed sequence. */
void reset_mission_inputs() noexcept {
    std::vector<MissionInputRecord>{}.swap(g_missionInputs);
    g_missionInputSequence = 0;
}

} // namespace detail

/** Reads the current accepted-mission-input position without replaying retained history. */
MissionInputCursor current_mission_input_cursor() noexcept {
    AcquireSRWLockShared(&g_lock);
    const MissionInputCursor cursor{g_eventGeneration, g_missionInputSequence};
    ReleaseSRWLockShared(&g_lock);
    return cursor;
}

/** Copies accepted client mission inputs after one cursor. */
void read_mission_inputs_after(MissionInputCursor after, MissionInputRead& output) noexcept {
    output = {};
    AcquireSRWLockExclusive(&g_lock);
    output.reset = after.generation != g_eventGeneration;
    const std::uint64_t afterSequence = output.reset ? 0 : after.sequence;
    retire_settled_mission_inputs();
    output.cursor = {g_eventGeneration, afterSequence};
    if (!g_missionInputs.empty()) {
        const std::uint64_t retainedPredecessor = g_missionInputs.front().view.sequence - 1;
        if (afterSequence < retainedPredecessor) {
            output.gap = true;
            output.missed = retainedPredecessor - afterSequence;
        }
        for (const MissionInputRecord& record : g_missionInputs) {
            if (record.view.sequence <= afterSequence) {
                continue;
            }
            if (output.count == output.events.size()) {
                break;
            }
            output.events[output.count++] = record.view;
        }
        if (output.count != 0) {
            output.cursor.sequence = output.events[output.count - 1].sequence;
        }
    } else if (afterSequence < g_missionInputSequence) {
        output.gap = true;
        output.missed = g_missionInputSequence - afterSequence;
        output.cursor.sequence = g_missionInputSequence;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Copies the exact Sense values owned by one retained accepted-input row. */
bool mission_input_sense_snapshot(std::uint64_t sequence,
                                  SenseObservationSnapshot& output) noexcept {
    namespace sense = middleware::bap::activity_message::sense_update;
    output = {};
    if (sequence == 0) {
        return false;
    }
    AcquireSRWLockShared(&g_lock);
    const MissionInputRecord* const selected = find_mission_input(sequence);
    bool copied = selected != nullptr && selected->hasSense
                  && selected->view.event.kind == EventKind::senseUpdate;
    if (copied) {
        const Event& event = selected->view.event;
        const sense::DecodedPacket& packet = selected->sense;
        copied =
            usable_sense_observation_packet({.sourceGeneration = event.sourceGeneration,
                                             .clientMessageSequence = event.clientMessageSequence,
                                             .verdict = event.verdict,
                                             .decodeStatus = event.senseDecodeStatus,
                                             .groupsSeen = event.groupsSeen,
                                             .groupsDecoded = event.groupsDecoded,
                                             .groupsSkipped = event.groupsSkipped,
                                             .objectsSeen = event.objectsSeen,
                                             .objectsDecoded = event.objectsDecoded},
                                            packet);
        if (copied) {
            output.revision = event.sequence;
            output.sourceGeneration = event.sourceGeneration;
        }
        for (std::size_t index = 0; copied && index < packet.objectCount; ++index) {
            const sense::DecodedObject& object = packet.objects[index];
            if (object.status != sense::ObjectStatus::decoded || !object.hasGeneration) {
                continue;
            }
            if (object.firstValue > packet.valueCount
                || object.valueCount > packet.valueCount - object.firstValue) {
                copied = false;
                break;
            }
            SenseObservation observation{};
            observation.binding = event.binding;
            observation.key = {object.registryKey,
                               object.objectTag,
                               object.senseSchema,
                               object.schemaRow,
                               object.slotIndex,
                               object.slotType};
            observation.sequence = event.sequence;
            observation.tick = event.tick;
            observation.sourceGeneration = event.sourceGeneration;
            observation.clientMessageSequence = event.clientMessageSequence;
            observation.generationPlusOne = object.generationPlusOne;
            observation.hasGeneration = object.hasGeneration;
            copied = append_sense_observation(
                output, observation, {packet.values.data() + object.firstValue, object.valueCount});
        }
    }
    if (!copied) {
        output = {};
    }
    ReleaseSRWLockShared(&g_lock);
    return copied;
}

/** Copies one exact generic client-message snapshot owned by the mission-input feed. */
bool mission_input_client_message_snapshot(std::uint64_t sequence,
                                           ClientMessageSnapshot& output) noexcept {
    output = {};
    if (sequence == 0) {
        return false;
    }
    AcquireSRWLockShared(&g_lock);
    const MissionInputRecord* const selected = find_mission_input(sequence);
    const bool copied = selected != nullptr && selected->hasClientMessage
                        && selected->view.event.kind == EventKind::clientMessageReceived;
    if (copied) {
        output = selected->clientMessage;
    }
    ReleaseSRWLockShared(&g_lock);
    return copied;
}

} // namespace sunrise::server::activity::host
