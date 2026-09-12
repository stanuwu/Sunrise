#pragma once

#include <cstddef>
#include <cstdint>

#include "../../state/activity_sdk/runtime.h"
#include "../bap/runtime.h"

namespace sunrise::server::activity::activity_sdk_mission {

/** Stable result of inspecting one automatically generated selected-state roster publication. */
enum class Status : std::uint8_t {
    ready,
    invalidView,
    staleBinding,
    staleActivityClient,
    noActivityLink,
    missingLiveSliceSet,
    wrongScenario,
    wrongSliceSet,
    missingInitialState,
    ambiguousInitialState,
    invalidOccurrence,
    schemaJoinNotExact,
    invalidRosterGroup,
    rosterKeyConflict,
    groupCapacityExceeded,
    outputBusy,
    refused,
};

/** Stable result of resolving or activating one generated authored-scene row. */
enum class SceneStatus : std::uint8_t {
    ready,
    queued,
    invalidView,
    staleBinding,
    staleActivityClient,
    noActivityLink,
    invalidOccurrence,
    invalidSlot,
    wrongScenario,
    wrongState,
    schemaJoinNotExact,
    missingResource,
    ambiguousResource,
    targetUnavailable,
    ambiguousTarget,
    missionSeedUnavailable,
    missionSeedPending,
    outputBusy,
    refused,
};

/** Exact generated plan plus its connection-scoped publication state. */
struct Snapshot final {
    server::bap::ActivityMissionSeedPlan plan{};
    std::uint64_t activityClientGeneration{};
    std::uint64_t revision{};
    std::uint64_t publishedRevision{};
    std::int32_t arrivalSliceSetIndex{-1};
    std::int32_t liveSliceSetIndex{-1};
    std::int32_t effectiveRegion{-1};
    bool configured{};
    bool publicationPending{};
    /** True while the selection's publication deliberately waits for the client's arrival. */
    bool regionArrivalPending{};
};

/** Resolves the generated plan and current lease without changing transport state. */
[[nodiscard]] Status query(const state::activity_sdk::BoundView& view, Snapshot& output) noexcept;

/**
 * Selects one authored effective region for this exact ActivityClient mission-seed lease.
 * @param omissions Objects this mission leaves out of the seed; the lease carries them onward.
 */
[[nodiscard]] Status
select_state(const state::activity_sdk::BoundView& view,
             std::int32_t effectiveRegion,
             std::span<const state::activity_sdk::MissionSeedOmission> omissions,
             Snapshot& output) noexcept;

/** Checks one exact occurrence and type-43 slot without changing transport state. */
[[nodiscard]] SceneStatus authored_scene_availability(const state::activity_sdk::BoundView& view,
                                                      std::uint32_t occurrenceRow,
                                                      std::uint32_t slotRow) noexcept;

/** Queues the next generation for one exact state-local authored scene. */
[[nodiscard]] SceneStatus activate_authored_scene(const state::activity_sdk::BoundView& view,
                                                  std::uint32_t occurrenceRow,
                                                  std::uint32_t slotRow) noexcept;

/** Checks one exact SDK-bounded type-53 authored dialogue cue. */
[[nodiscard]] SceneStatus dialogue_cue_availability(const state::activity_sdk::BoundView& view,
                                                    std::uint32_t occurrenceRow,
                                                    std::uint32_t slotRow,
                                                    std::uint16_t cueIndex) noexcept;

/** Queues one authored dialogue cue through its native type-53 consumer. */
[[nodiscard]] SceneStatus play_dialogue_cue(const state::activity_sdk::BoundView& view,
                                            std::uint32_t occurrenceRow,
                                            std::uint32_t slotRow,
                                            std::uint16_t cueIndex) noexcept;

/** Checks one exact generated type-68 HUD directive element. */
[[nodiscard]] SceneStatus directive_availability(const state::activity_sdk::BoundView& view,
                                                 std::uint32_t occurrenceRow,
                                                 std::uint32_t slotRow,
                                                 std::uint32_t nameHash,
                                                 std::int32_t elementIndex) noexcept;

/** Shows or hides one exact generated type-68 HUD directive. */
[[nodiscard]] SceneStatus set_directive(const state::activity_sdk::BoundView& view,
                                        std::uint32_t occurrenceRow,
                                        std::uint32_t slotRow,
                                        std::uint32_t nameHash,
                                        std::int32_t elementIndex,
                                        std::int8_t state,
                                        bool visible) noexcept;

/** Checks one exact SDK-linked type-38 authored task. */
[[nodiscard]] SceneStatus objective_reset_availability(const state::activity_sdk::BoundView& view,
                                                       std::uint32_t occurrenceRow,
                                                       std::uint32_t slotRow) noexcept;

/** Resets every objective and its client-owned task counters. */
[[nodiscard]] SceneStatus reset_objectives(const state::activity_sdk::BoundView& view,
                                           std::uint32_t occurrenceRow,
                                           std::uint32_t slotRow) noexcept;

/** Checks the current occurrence for one exact type-3 objective-reset Slot row. */
[[nodiscard]] SceneStatus
objective_reset_slot_availability(const state::activity_sdk::BoundView& view,
                                  std::uint32_t slotRow) noexcept;

/** Queues one type-3 objective reset against an already assigned Host output revision. */
[[nodiscard]] SceneStatus
reset_objectives_slot_reserved(const state::activity_sdk::BoundView& view,
                               std::uint32_t slotRow,
                               const host::ScriptableOutputReservation& reservation) noexcept;

/** Checks one exact SDK-linked type-38 authored task. */
[[nodiscard]] SceneStatus task_availability(const state::activity_sdk::BoundView& view,
                                            std::uint32_t occurrenceRow,
                                            std::uint32_t slotRow) noexcept;

/** Queues one exact authored task through its native type-38 consumer. */
[[nodiscard]] SceneStatus activate_task(const state::activity_sdk::BoundView& view,
                                        std::uint32_t occurrenceRow,
                                        std::uint32_t slotRow) noexcept;

/** Checks one exact SDK-linked type-5 authored sequence. */
[[nodiscard]] SceneStatus sequence_availability(const state::activity_sdk::BoundView& view,
                                                std::uint32_t occurrenceRow,
                                                std::uint32_t slotRow) noexcept;

/** Restarts one exact authored sequence through its native type-5 consumer. */
[[nodiscard]] SceneStatus play_sequence(const state::activity_sdk::BoundView& view,
                                        std::uint32_t occurrenceRow,
                                        std::uint32_t slotRow) noexcept;

/** Checks one exact SDK-linked type-6 authored cinematic. */
[[nodiscard]] SceneStatus cinematic_availability(const state::activity_sdk::BoundView& view,
                                                 std::uint32_t occurrenceRow,
                                                 std::uint32_t slotRow) noexcept;

/** Starts or stops one exact authored cinematic through its native consumer. */
[[nodiscard]] SceneStatus set_cinematic_active(const state::activity_sdk::BoundView& view,
                                               std::uint32_t occurrenceRow,
                                               std::uint32_t slotRow,
                                               bool active) noexcept;

[[nodiscard]] SceneStatus
play_sequence_reserved(const state::activity_sdk::BoundView& view,
                       std::uint32_t occurrenceRow,
                       std::uint32_t slotRow,
                       const host::ScriptableOutputReservation& reservation) noexcept;

[[nodiscard]] SceneStatus
set_cinematic_active_reserved(const state::activity_sdk::BoundView& view,
                              std::uint32_t occurrenceRow,
                              std::uint32_t slotRow,
                              bool active,
                              const host::ScriptableOutputReservation& reservation) noexcept;

/** Lua-slot variant: resolves the unique occurrence in the currently published mission state. */
[[nodiscard]] SceneStatus sequence_slot_availability(const state::activity_sdk::BoundView& view,
                                                     std::uint32_t slotRow) noexcept;
[[nodiscard]] SceneStatus
play_sequence_slot_reserved(const state::activity_sdk::BoundView& view,
                            std::uint32_t slotRow,
                            const host::ScriptableOutputReservation& reservation) noexcept;
[[nodiscard]] SceneStatus cinematic_slot_availability(const state::activity_sdk::BoundView& view,
                                                      std::uint32_t slotRow) noexcept;
[[nodiscard]] SceneStatus
set_cinematic_slot_active_reserved(const state::activity_sdk::BoundView& view,
                                   std::uint32_t slotRow,
                                   bool active,
                                   const host::ScriptableOutputReservation& reservation) noexcept;
/** Starts one state of the actor a type-42 performance sensor drives, for an operator. */
[[nodiscard]] SceneStatus play_performance_slot(const state::activity_sdk::BoundView& view,
                                                std::uint32_t slotRow,
                                                std::uint32_t stateNameHash) noexcept;
/** Starts one state of the actor a type-42 performance sensor drives. */
[[nodiscard]] SceneStatus
play_performance_slot_reserved(const state::activity_sdk::BoundView& view,
                               std::uint32_t slotRow,
                               std::uint32_t stateNameHash,
                               const host::ScriptableOutputReservation& reservation) noexcept;

[[nodiscard]] SceneStatus task_slot_availability(const state::activity_sdk::BoundView& view,
                                                 std::uint32_t slotRow) noexcept;
[[nodiscard]] SceneStatus
activate_task_slot_reserved(const state::activity_sdk::BoundView& view,
                            std::uint32_t slotRow,
                            const host::ScriptableOutputReservation& reservation) noexcept;
[[nodiscard]] SceneStatus dialogue_cue_slot_availability(const state::activity_sdk::BoundView& view,
                                                         std::uint32_t slotRow,
                                                         std::uint16_t cueIndex) noexcept;
[[nodiscard]] SceneStatus
play_dialogue_cue_slot_reserved(const state::activity_sdk::BoundView& view,
                                std::uint32_t slotRow,
                                std::uint16_t cueIndex,
                                const host::ScriptableOutputReservation& reservation) noexcept;

/** Queues only through the exact unarmed Host revision owned by Mission State. */
[[nodiscard]] SceneStatus
activate_authored_scene_reserved(const state::activity_sdk::BoundView& view,
                                 std::uint32_t occurrenceRow,
                                 std::uint32_t slotRow,
                                 const host::ScriptableOutputReservation& reservation,
                                 std::uint32_t eventKey = 0,
                                 bool stop = false) noexcept;

/** @return Stable concise text for one mission runtime result. */
[[nodiscard]] const char* status_name(Status status) noexcept;

/** @return Stable concise text for one authored-scene result. */
[[nodiscard]] const char* status_name(SceneStatus status) noexcept;

} // namespace sunrise::server::activity::activity_sdk_mission
