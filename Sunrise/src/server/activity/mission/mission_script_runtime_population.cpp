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
    std::span<const middleware::bap::activity_message::sense_update::DecodedValue> values,
    std::optional<std::int32_t> linkAlive,
    bool moved) noexcept {
    if (instance.dispatchAttemptGeneration == 0 || instance.dispatchInputSequence == 0) {
        return true;
    }
    mission_state::SquadPopulationReport report{};
    report.attemptGeneration = instance.dispatchAttemptGeneration;
    report.sourceGeneration = observation.sourceGeneration;
    // Every report reaching here is the simulating link's, so each one may move the population.
    report.admitted = true;
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
    // A placement does not make the client restate an unchanged count, so the link's level holds.
    report.aliveKnown = linkAlive.has_value();
    report.alive = linkAlive.value_or(0);
    std::int32_t restated = 0;
    const bool aliveRestated = read_squad_alive(values, observation.key.schemaRow, restated);
    std::array<std::int32_t, kSquadLostSlotCapacity> lost{};
    if (read_squad_lost_counts(values, lost) != 0) {
        const std::int64_t total = std::accumulate(lost.begin(), lost.end(), std::int64_t{0});
        report.lostKnown = total <= kMaximumCounter;
        report.lost = static_cast<std::int32_t>(report.lostKnown ? total : 0);
    }
    if (!moved && !aliveRestated && !report.generationKnown && !report.lostKnown) {
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
