#include <array>
#include <limits>
#include <numeric>
#include <span>

#include "mission_script_runtime_internal.h"

namespace sunrise::server::activity::mission {

/** Retains only accepted generation and alive fields for a transported squad placement. */
bool observe_population(
    RuntimeInstance& instance,
    const host::SenseObservation& observation,
    std::span<const middleware::bap::activity_message::sense_update::DecodedValue>
        values) noexcept {
    if (instance.dispatchAttemptGeneration == 0 || instance.dispatchInputSequence == 0) {
        return true;
    }
    mission_state::SquadPopulationReport report{};
    report.attemptGeneration = instance.dispatchAttemptGeneration;
    report.sourceGeneration = observation.sourceGeneration;
    report.inputSequence = instance.dispatchInputSequence;
    report.clientMessageSequence = observation.clientMessageSequence;
    report.objectTag = observation.key.objectTag;
    report.registryKey = observation.key.registryKey;
    report.slotIndex = observation.key.slotIndex;
    // Type-1 root field zero echoes its 31-bit spawn generation.
    constexpr std::uint16_t kSpawnGenerationOrdinal = 0;
    // A replica that has not applied Auth cannot replace the owned population.
    constexpr std::uint16_t kInitializedOrdinal = 10;
    for (const auto& value : values) {
        if (!value.present || value.schemaRow != observation.key.schemaRow
            || value.occurrence != 0) {
            continue;
        }
        if (value.fieldOrdinal == kSpawnGenerationOrdinal) {
            if (value.unsignedValue > kMaximumCounter) {
                return true;
            }
            report.spawnGeneration = static_cast<std::uint32_t>(value.unsignedValue);
            report.generationKnown = true;
        } else if (value.fieldOrdinal == kInitializedOrdinal) {
            report.initialized =
                value.kind == middleware::bap::activity_message::sense_update::ValueKind::boolean
                && value.unsignedValue == 1;
        }
    }
    report.aliveKnown = read_squad_alive(values, observation.key.schemaRow, report.alive);
    std::array<std::int32_t, kSquadCreatedSlotCapacity> created{};
    if (read_squad_created_counts(values, created) != 0) {
        const std::int64_t total = std::accumulate(created.begin(), created.end(), std::int64_t{0});
        report.createdKnown = total <= kMaximumCounter;
        report.created = static_cast<std::int32_t>(report.createdKnown ? total : 0);
    }
    if (!report.aliveKnown && !report.generationKnown && !report.createdKnown) {
        return true;
    }
    mission_state::Snapshot snapshot{};
    const auto status = mission_state::observe_squad_population(
        instance.view.binding, instance.programKey, report, snapshot);
    if (status != mission_state::Status::ready) {
        fault_instance(instance, "accepted population report could not update native state");
        return false;
    }
    accept_mission_state(instance, snapshot);
    return true;
}

} // namespace sunrise::server::activity::mission
