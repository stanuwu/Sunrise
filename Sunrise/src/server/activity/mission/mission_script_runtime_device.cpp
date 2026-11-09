#include <array>
#include <cstdio>
#include <span>

#include "mission_script_runtime_internal.h"

namespace sunrise::server::activity::mission {
namespace {

/**
 * Retains a device under its complete authored slot identity.
 * @param instance Bound mission instance.
 * @param key Exact accepted Sense source.
 * @return The matching or newly allocated row, or null at capacity.
 */
[[nodiscard]] DeviceObservation*
find_device_observation(RuntimeInstance& instance, const host::SenseObservationKey& key) noexcept {
    DeviceObservation* spare = nullptr;
    for (auto& row : instance.deviceObservations) {
        if (row.used && row.registryKey == key.registryKey && row.objectTag == key.objectTag
            && row.slotIndex == key.slotIndex) {
            return &row;
        }
        if (!row.used && spare == nullptr) {
            spare = &row;
        }
    }
    if (spare != nullptr) {
        spare->registryKey = key.registryKey;
        spare->objectTag = key.objectTag;
        spare->slotIndex = key.slotIndex;
    }
    return spare;
}

} // namespace

/**
 * Publishes current device levels only when accepted client reports change them. Every client
 * that holds a device body applies it, so only the link that simulates the device reports for it.
 * @param instance Bound mission instance retaining previous device levels.
 * @param sense Accepted observations of one link.
 */
void push_device_edges(RuntimeInstance& instance,
                       const host::SenseObservationSnapshot& sense) noexcept {
    if (sense.observationCount > sense.observations.size()
        || sense.valueCount > sense.values.size()) {
        return;
    }
    for (std::size_t index = 0; index < sense.observationCount; ++index) {
        const auto& observation = sense.observations[index];
        if (observation.sourceGeneration != sense.sourceGeneration
            || observation.key.slotType != kDeviceSenseSlotType
            || observation.key.senseSchema != kDeviceSenseSchema
            || observation.firstValue > sense.valueCount
            || observation.valueCount > sense.valueCount - observation.firstValue
            || !admitted_report(instance, observation.key, sense.sourceGeneration)) {
            continue;
        }
        auto* const row = find_device_observation(instance, observation.key);
        if (row == nullptr) {
            log_line(core::log::Level::warn, &instance, "device_state", "watch_capacity");
            continue;
        }
        claim_row(*row, sense.sourceGeneration, false);
        const bool first = !row->level.observed;
        bool reset = false;
        const auto body =
            std::span(sense.values).subspan(observation.firstValue, observation.valueCount);
        DeviceLevel incoming{};
        bool ignoredReset = false;
        if (!update_device_level(incoming, body, observation.key.schemaRow, ignoredReset)) {
            continue;
        }
        const bool changed =
            update_device_level(row->level, body, observation.key.schemaRow, reset);
        row->used = true;
        mission_state::DeviceReport report{};
        report.attemptGeneration = instance.dispatchAttemptGeneration;
        report.sourceGeneration = sense.sourceGeneration;
        report.inputSequence = instance.dispatchInputSequence;
        report.clientMessageSequence = observation.clientMessageSequence;
        report.objectTag = observation.key.objectTag;
        report.registryKey = observation.key.registryKey;
        report.slotIndex = observation.key.slotIndex;
        report.admitted = true;
        for (std::size_t channel = 0; channel < incoming.channels.size(); ++channel) {
            const auto& value = incoming.channels[channel];
            report.channels[channel] = {
                value.value, value.sequence, value.valueKnown, value.sequenceKnown};
        }
        mission_state::Snapshot snapshot{};
        std::array<std::uint64_t, mission_state::kDeviceChannelCount> applied{};
        const auto status = mission_state::observe_device_report(
            instance.view.binding, instance.programKey, report, applied, snapshot);
        if (status != mission_state::Status::ready) {
            fault_instance(instance, "accepted device report could not join its native request");
            return;
        }
        accept_mission_state(instance, snapshot);
        if (!changed
            && std::all_of(applied.begin(), applied.end(), [](auto key) { return key == 0; })) {
            continue;
        }
        host::Event event{};
        event.attemptGeneration = report.attemptGeneration;
        event.deviceAppliedRequests = applied;
        event.kind = host::EventKind::deviceState;
        event.binding = instance.view.binding;
        event.sequence = observation.sequence;
        event.tick = observation.tick;
        event.sourceGeneration = instance.view.activityClientGeneration;
        event.missionSequence = report.inputSequence;
        event.firstRegistryKey = observation.key.registryKey;
        event.firstSlotIndex = observation.key.slotIndex;
        event.firstSlotType = observation.key.slotType;
        event.slotObjectTag = observation.key.objectTag;
        event.slotSenseSchema = observation.key.senseSchema;
        event.deviceFirstReport = first;
        event.deviceReset = reset;
        for (std::size_t channel = 0; channel < row->level.channels.size(); ++channel) {
            const auto& level = row->level.channels[channel];
            event.deviceValues[channel] = level.value;
            event.deviceSequences[channel] = level.sequence;
            if (level.valueKnown) {
                event.deviceValueMask |= static_cast<std::uint8_t>(1U << channel);
            }
            if (level.sequenceKnown) {
                event.deviceSequenceMask |= static_cast<std::uint8_t>(1U << channel);
            }
        }
        std::array<char, 128> details{};
        const int written = std::snprintf(details.data(),
                                          details.size(),
                                          "registry=%08X slot=%u position=%.4f sequence=%d "
                                          "known=%u reset=%u",
                                          observation.key.registryKey,
                                          static_cast<unsigned>(observation.key.slotIndex),
                                          static_cast<double>(event.deviceValues[0]),
                                          event.deviceSequences[0],
                                          static_cast<unsigned>(event.deviceValueMask),
                                          reset ? 1U : 0U);
        if (written > 0 && static_cast<std::size_t>(written) < details.size()) {
            log_line(core::log::Level::debug,
                     &instance,
                     "device_state",
                     "changed",
                     {details.data(), static_cast<std::size_t>(written)});
        }
        push_script_event(instance, event);
    }
}

} // namespace sunrise::server::activity::mission
