#include <Windows.h>

#include <algorithm>
#include <limits>
#include <new>
#include <span>
#include <utility>

#include "../../runtime/storage/internal.h"
#include "internal.h"

namespace sunrise::state::activity::mission {
namespace {

/** A desired placement stays unknown until its exact output and population are reported. */
Status prepare_population_row(MissionState& candidate,
                              const TypedIntent& intent,
                              std::uint32_t squadRow) noexcept {
    SquadPopulation desired{};
    desired.requestKey = intent.requestKey;
    desired.attemptGeneration = intent.attemptGeneration;
    desired.squadRow = squadRow;
    for (auto& population : candidate.squadPopulations) {
        if (population.squadRow == desired.squadRow) {
            if (population.requestKey <= desired.requestKey) {
                population = desired;
            }
            return Status::ready;
        }
    }
    try {
        candidate.squadPopulations.push_back(desired);
    } catch (const std::bad_alloc&) {
        return Status::outOfMemory;
    }
    return Status::ready;
}

/** Replaces one exact source's count evidence without changing a newer desired owner. */
Status retain_population(MissionState& candidate,
                         const TypedIntent& intent,
                         const SquadPopulation& population,
                         std::int32_t expectedAlive) noexcept {
    SquadPopulation next = population;
    next.requestKey = intent.requestKey;
    next.expectedAlive = expectedAlive;
    next.alive = next.maximumObservedAlive = next.maximumObservedCreated = 0;
    next.lastInputSequence = 0;
    next.generationKnown = next.aliveKnown = false;
    for (auto& existing : candidate.squadPopulations) {
        if (existing.squadRow == next.squadRow) {
            if (existing.requestKey <= next.requestKey) {
                existing = next;
            }
            return Status::ready;
        }
    }
    try {
        candidate.squadPopulations.push_back(next);
    } catch (const std::bad_alloc&) {
        return Status::outOfMemory;
    }
    return Status::ready;
}

/** @return True when one report names the placement one population counts. */
[[nodiscard]] bool reports_population(const SquadPopulation& population,
                                      const SquadPopulationReport& report) noexcept {
    return population.attemptGeneration == report.attemptGeneration
           && population.spawnGeneration != 0 && population.objectTag == report.objectTag
           && population.registryKey == report.registryKey
           && population.slotIndex == report.slotIndex;
}

/** @return True when one admitted report would move a population another link holds. */
[[nodiscard]] bool moves_population(const MissionState& mission,
                                    const SquadPopulationReport& report) noexcept {
    return report.admitted && report.sourceGeneration != 0
           && std::any_of(mission.squadPopulations.begin(),
                          mission.squadPopulations.end(),
                          [&report](const SquadPopulation& population) noexcept {
                              return reports_population(population, report)
                                     && population.sourceGeneration != report.sourceGeneration;
                          });
}

} // namespace

/** Queueing scene creation invalidates every captured parent before any output is staged. */
Status prepare_squad_population(MissionState& candidate, const TypedIntent& intent) noexcept {
    if (intent.kind == IntentKind::activateAuthoredScene && intent.active) {
        if (intent.burstRowCount > intent.burstRows.size()) {
            return Status::intentMismatch;
        }
        for (std::size_t index = 0; index < intent.burstRowCount; ++index) {
            const auto previous = std::span(intent.burstRows).first(index);
            if (std::find(previous.begin(), previous.end(), intent.burstRows[index])
                != previous.end()) {
                return Status::intentMismatch;
            }
            const auto status = prepare_population_row(candidate, intent, intent.burstRows[index]);
            if (status != Status::ready) {
                return status;
            }
        }
        return Status::ready;
    }
    return prepare_population_row(candidate,
                                  intent,
                                  intent.kind == IntentKind::runActorProgram ? intent.secondRow
                                                                             : intent.firstRow);
}

/** Scene source counts use the final stage boundary and carry no full-population expectation. */
Status retain_scene_populations(MissionState& candidate,
                                const TypedIntent& intent,
                                std::span<const SquadPopulation> populations) noexcept {
    if (intent.kind != IntentKind::activateAuthoredScene || !intent.active
        || intent.burstRowCount > intent.burstRows.size()
        || populations.size() != intent.burstRowCount) {
        return Status::intentMismatch;
    }
    for (std::size_t index = 0; index < populations.size(); ++index) {
        const auto& population = populations[index];
        if (population.squadRow != intent.burstRows[index] || population.spawnGeneration == 0
            || population.sourceGeneration == 0
            || population.attemptGeneration != intent.attemptGeneration
            || population.attemptGeneration != candidate.attempt.generation) {
            return Status::intentMismatch;
        }
        const auto status = retain_population(candidate, intent, population, 0);
        if (status != Status::ready) {
            return status;
        }
    }
    return Status::ready;
}

/** A new transported placement replaces the old generation's count evidence. */
Status retain_squad_population(MissionState& candidate,
                               const TypedIntent& intent,
                               const SquadPopulation& population) noexcept {
    if (intent.kind != IntentKind::placeSquad || intent.squadCount > intent.squadCounts.size()
        || population.squadRow != intent.firstRow || population.spawnGeneration == 0
        || population.sourceGeneration == 0
        || population.attemptGeneration != intent.attemptGeneration
        || population.attemptGeneration != candidate.attempt.generation) {
        return Status::intentMismatch;
    }
    std::int64_t expected = 0;
    for (std::size_t index = 0; index < intent.squadCount; ++index) {
        if (intent.squadCounts[index] < 0) {
            return Status::intentMismatch;
        }
        expected += intent.squadCounts[index];
    }
    if (expected > (std::numeric_limits<std::int32_t>::max)()) {
        return Status::intentMismatch;
    }
    return retain_population(candidate, intent, population, static_cast<std::int32_t>(expected));
}

/**
 * Count evidence cannot cross an attempt, placement generation, source link or accepted-input
 * boundary. Another link's report of the same squad is a replica's, not the placement's, unless
 * it is admitted as the squad's simulation: the evidence gathered so far then follows that link.
 * Any admitted report moves it, so a move a report could not carry is made by the next one.
 */
Status observe_squad_population(const SessionBinding& binding,
                                const ProgramKey& program,
                                const SquadPopulationReport& report,
                                Snapshot& output) noexcept {
    output = {};
    if ((report.aliveKnown && (report.alive < 0 || report.alive > kMaximumSquadAlive))
        || (report.createdKnown && report.created < 0)) {
        return Status::invalidTransition;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    auto& activity = runtime::storage::g_state.activity;
    auto* record = find_record(activity, binding);
    Status status = record == nullptr ? Status::invalidBinding : Status::ready;
    if (status == Status::ready
        && (!record->mission.programBound || !same_program(record->mission.program, program))) {
        status = Status::programMismatch;
    }
    if (status == Status::ready
        && (record->mission.faulted || report.inputSequence == 0
            || report.inputSequence > record->mission.issuedInputSequence)) {
        status = Status::invalidTransition;
    }
    // Only an initialized report or a move can change a population, so no other report copies the
    // state under the exclusive lock.
    if (status == Status::ready && report.attemptGeneration == record->mission.attempt.generation
        && (report.initialized || moves_population(record->mission, report))) {
        MissionState candidate{};
        if (!copy_mission_state(record->mission, candidate)) {
            status = Status::outOfMemory;
        } else {
            bool changed = false;
            for (auto& population : candidate.squadPopulations) {
                if (!reports_population(population, report)) {
                    continue;
                }
                if (population.sourceGeneration != report.sourceGeneration) {
                    if (!report.admitted || report.sourceGeneration == 0) {
                        continue;
                    }
                    population.sourceGeneration = report.sourceGeneration;
                    changed = true;
                }
                if (!report.initialized || report.inputSequence <= population.inputSequenceAtStage
                    || report.clientMessageSequence <= population.clientMessageSequenceAtStage
                    || report.inputSequence <= population.lastInputSequence) {
                    continue;
                }
                if (report.generationKnown
                    && report.spawnGeneration != population.spawnGeneration) {
                    population.maximumObservedCreated = 0;
                    population.generationKnown = population.aliveKnown = false;
                    population.lastInputSequence = report.inputSequence;
                    changed = true;
                    continue;
                }
                if (!population.generationKnown && !report.generationKnown) {
                    continue;
                }
                population.generationKnown = true;
                population.lastInputSequence = report.inputSequence;
                if (report.aliveKnown) {
                    population.alive = report.alive;
                    population.aliveKnown = true;
                    population.maximumObservedAlive =
                        (std::max)(population.maximumObservedAlive, report.alive);
                }
                if (report.createdKnown) {
                    population.maximumObservedCreated =
                        (std::max)(population.maximumObservedCreated, report.created);
                }
                changed = true;
            }
            if (changed) {
                if (!publish(activity, *record)) {
                    status = Status::revisionExhausted;
                } else {
                    record->mission = std::move(candidate);
                }
            }
        }
    }
    if (status == Status::ready && !copy_snapshot(activity, binding, *record, output)) {
        status = Status::outOfMemory;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return status;
}

} // namespace sunrise::state::activity::mission
