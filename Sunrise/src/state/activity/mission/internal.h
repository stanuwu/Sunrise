#pragma once

#include <cstdint>

#include "runtime.h"

namespace sunrise::state::activity::mission {

/** @return True when every hash, tag and index of a program key is set. */
[[nodiscard]] bool valid_program(const ProgramKey& program) noexcept;

/** @return True when two program keys name the same bound script. */
[[nodiscard]] bool same_program(const ProgramKey& left, const ProgramKey& right) noexcept;

/** @return The record this exact binding owns, or null when no session matches. */
[[nodiscard]] SessionRecord* find_record(ActivityState& state,
                                         const SessionBinding& binding) noexcept;

/** @return The record this exact binding owns, or null when no session matches. */
[[nodiscard]] const SessionRecord* find_record(const ActivityState& state,
                                               const SessionBinding& binding) noexcept;

/** Publishes one committed record to the caller. @return False when the copy fails. */
[[nodiscard]] bool copy_snapshot(const ActivityState& activity,
                                 const SessionBinding&,
                                 const SessionRecord& record,
                                 Snapshot& output) noexcept;

/** Copies one durable mission value. @return False when the copy fails. */
[[nodiscard]] bool copy_mission_state(const MissionState& source, MissionState& output) noexcept;

/** Validates and installs the population expectation owned by a staged squad intent. */
[[nodiscard]] Status retain_squad_population(MissionState& candidate,
                                             const TypedIntent& intent,
                                             const SquadPopulation& population) noexcept;

/** A new desired placement removes the earlier placement's clearance proof. */
[[nodiscard]] Status prepare_squad_population(MissionState& candidate,
                                              const TypedIntent& intent) noexcept;

/** Scene-created actors admit fresh parent counts without proving that each child was born. */
[[nodiscard]] Status
retain_scene_populations(MissionState& candidate,
                         const TypedIntent& intent,
                         std::span<const SquadPopulation> populations) noexcept;

/** Appends one intent to the durable queue with its sequence and the tables it seeds. */
[[nodiscard]] Status append_intent(MissionState& candidate,
                                   const TypedIntent& intent,
                                   std::uint64_t missionRevision) noexcept;

/** Takes the next Activity State revision for one record. @return False when none is left. */
[[nodiscard]] bool publish(ActivityState& activity, SessionRecord& record) noexcept;

/**
 * Reads the durable head intent after the shared delivery guards pass.
 * @param output Receives the head intent, null unless the return is ready.
 */
[[nodiscard]] Status checked_head(SessionRecord& record,
                                  const ProgramKey& program,
                                  std::uint64_t expectedMissionRevision,
                                  std::uint64_t expectedIntentSequence,
                                  PendingIntent*& output) noexcept;

} // namespace sunrise::state::activity::mission
