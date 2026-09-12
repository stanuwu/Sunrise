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
 * Publishes current device levels only when accepted client reports change them.
 * @param instance Bound mission instance retaining previous device levels.
 * @param sense Accepted observations for its current client generation.
 */
void push_device_edges(RuntimeInstance& instance,
                       const host::SenseObservationSnapshot& sense) noexcept {
    if (sense.sourceGeneration != instance.view.activityClientGeneration
        || sense.observationCount > sense.observations.size()
        || sense.valueCount > sense.values.size()) {
        return;
    }
    for (std::size_t index = 0; index < sense.observationCount; ++index) {
        const auto& observation = sense.observations[index];
        if (observation.sourceGeneration != sense.sourceGeneration
            || observation.key.slotType != kDeviceSenseSlotType
            || observation.key.senseSchema != kDeviceSenseSchema
            || observation.firstValue > sense.valueCount
            || observation.valueCount > sense.valueCount - observation.firstValue) {
            continue;
        }
        auto* const row = find_device_observation(instance, observation.key);
        if (row == nullptr) {
            log_line(core::log::Level::warn, &instance, "device_state", "watch_capacity");
            continue;
        }
        const bool first = !row->level.observed;
        bool reset = false;
        const auto body =
            std::span(sense.values).subspan(observation.firstValue, observation.valueCount);
        if (!update_device_level(row->level, body, observation.key.schemaRow, reset)) {
            continue;
        }
        row->used = true;
        host::Event event{};
        event.kind = host::EventKind::deviceState;
        event.binding = instance.view.binding;
        event.sequence = observation.sequence;
        event.tick = observation.tick;
        event.sourceGeneration = instance.view.activityClientGeneration;
        event.missionSequence = instance.lastMissionSequence;
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
