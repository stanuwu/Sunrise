#include <Windows.h>

#include <array>
#include <limits>
#include <span>

#include "../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../state/activity/runtime.h"
#include "activity_sdk_behavior_scope.h"
#include "activity_sdk_mission_internal.h"
#include "activity_sdk_scriptable_route.h"
#include "host_runtime.h"

namespace sunrise::server::activity::activity_sdk_mission {
namespace {

namespace layouts = state::build_data::scenarios;
namespace message = middleware::bap::activity_message::sensor_auth_update;
namespace sdk = state::activity_sdk;
namespace sdk_route = server::activity::activity_sdk_scriptables;

using detail::binding_status;
using detail::lease_status;
using detail::materialize_plan;
using detail::prepare_dialogue;
using detail::prepare_directive;
using detail::prepare_objective;
using detail::prepare_scene;
using detail::prepare_task;
using detail::prepare_typed_behavior;
using detail::PreparedScene;
using detail::read_lease;
using detail::same_plan;
using detail::scene_binding_status;

} // namespace

/** Resolves the generated plan and current lease without changing transport state. */
Status query(const sdk::BoundView& view, Snapshot& output) noexcept {
    output = {};
    server::bap::ActivityLinkView link{};
    const Status binding = binding_status(view, link);
    if (binding != Status::ready) {
        return binding;
    }
    output.activityClientGeneration = link.activityClientGeneration;
    output.arrivalSliceSetIndex = link.arrivalSliceSetIndex;
    output.liveSliceSetIndex = link.sliceSetIndex;
    output.effectiveRegion = link.effectiveRegion;

    server::bap::ActivityMissionSeedLeaseView lease{};
    const Status leaseResult = read_lease(view, link, lease);
    if (leaseResult != Status::ready) {
        return leaseResult;
    }
    output.revision = lease.revision;
    output.publishedRevision = lease.publishedRevision;
    output.configured = lease.configured;
    output.publicationPending = lease.publicationPending;
    output.regionArrivalPending = lease.regionArrivalPending;

    server::bap::ActivityMissionSeedPlan generated{};
    const std::int32_t selectedRegion =
        lease.configured
                && lease.plan.effectiveRegion
                       <= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())
            ? static_cast<std::int32_t>(lease.plan.effectiveRegion)
            : link.effectiveRegion;
    // The lease's own omissions produced its plan, so the check must re-materialize with them.
    const Status generatedStatus = materialize_plan(
        view,
        selectedRegion,
        std::span(lease.plan.omissions).first(lease.configured ? lease.plan.omissionCount : 0),
        generated);
    output.plan = lease.configured ? lease.plan : generated;
    if (generatedStatus != Status::ready) {
        return generatedStatus;
    }
    if (lease.configured && !same_plan(lease.plan, generated)) {
        return lease.plan.effectiveRegion == generated.effectiveRegion ? Status::refused
                                                                       : Status::wrongSliceSet;
    }
    return Status::ready;
}

/** Selects one exact authored state and exposes its asynchronous publication state. */
Status select_state(const sdk::BoundView& view,
                    std::int32_t effectiveRegion,
                    std::span<const sdk::MissionSeedOmission> omissions,
                    Snapshot& output) noexcept {
    output = {};
    server::bap::ActivityLinkView link{};
    const Status binding = binding_status(view, link);
    if (binding != Status::ready) {
        return binding;
    }
    server::bap::ActivityMissionSeedPlan plan{};
    const Status materialized = materialize_plan(view, effectiveRegion, omissions, plan);
    if (materialized != Status::ready) {
        return materialized;
    }
    const Status selected = lease_status(server::bap::select_activity_mission_seed(
        view.binding, plan, link.activityClientGeneration));
    if (selected != Status::ready) {
        return selected;
    }
    return query(view, output);
}

/** Checks one exact occurrence and type-43 slot without changing transport state. */
SceneStatus authored_scene_availability(const sdk::BoundView& view,
                                        std::uint32_t occurrenceRow,
                                        std::uint32_t slotRow) noexcept {
    PreparedScene prepared{};
    return prepare_scene(view, occurrenceRow, slotRow, prepared);
}

/** Queues the next generation for one exact state-local authored scene. */
SceneStatus activate_authored_scene(const sdk::BoundView& view,
                                    std::uint32_t occurrenceRow,
                                    std::uint32_t slotRow) noexcept {
    PreparedScene prepared{};
    const SceneStatus status = prepare_scene(view, occurrenceRow, slotRow, prepared);
    if (status != SceneStatus::ready) {
        return status;
    }
    if (server::bap::request_activity_state_local_authored_scene_override(
            view.binding,
            prepared.target,
            prepared.rosterGroup,
            prepared.effectiveRegion,
            prepared.activityClientGeneration,
            nullptr,
            prepared.sceneDependencies)) {
        return SceneStatus::queued;
    }
    return SceneStatus::refused;
}

/** Checks one exact SDK-bounded authored dialogue cue. */
SceneStatus dialogue_cue_availability(const sdk::BoundView& view,
                                      std::uint32_t occurrenceRow,
                                      std::uint32_t slotRow,
                                      std::uint16_t cueIndex) noexcept {
    PreparedScene prepared{};
    std::uint16_t authoredCueCount = 0;
    return prepare_dialogue(view, occurrenceRow, slotRow, cueIndex, prepared, authoredCueCount);
}

/** Queues one authored dialogue cue through the dedicated type-53 encoder. */
SceneStatus play_dialogue_cue(const sdk::BoundView& view,
                              std::uint32_t occurrenceRow,
                              std::uint32_t slotRow,
                              std::uint16_t cueIndex) noexcept {
    PreparedScene prepared{};
    std::uint16_t authoredCueCount = 0;
    const SceneStatus status =
        prepare_dialogue(view, occurrenceRow, slotRow, cueIndex, prepared, authoredCueCount);
    if (status != SceneStatus::ready) {
        return status;
    }
    if (server::bap::request_activity_state_local_dialogue_override(
            view.binding,
            prepared.target,
            prepared.rosterGroup,
            cueIndex,
            authoredCueCount,
            prepared.effectiveRegion,
            prepared.activityClientGeneration)) {
        return SceneStatus::queued;
    }
    return SceneStatus::refused;
}

SceneStatus directive_availability(const sdk::BoundView& view,
                                   std::uint32_t occurrenceRow,
                                   std::uint32_t slotRow,
                                   std::uint32_t nameHash,
                                   std::int32_t elementIndex) noexcept {
    PreparedScene prepared{};
    return prepare_directive(view, occurrenceRow, slotRow, nameHash, elementIndex, prepared);
}

/** Sets one HUD directive element's state and visibility on a prepared objective slot. */
SceneStatus set_directive(const sdk::BoundView& view,
                          std::uint32_t occurrenceRow,
                          std::uint32_t slotRow,
                          std::uint32_t nameHash,
                          std::int32_t elementIndex,
                          std::int8_t state,
                          bool visible) noexcept {
    PreparedScene prepared{};
    const SceneStatus status =
        prepare_directive(view, occurrenceRow, slotRow, nameHash, elementIndex, prepared);
    if (status != SceneStatus::ready) {
        return status;
    }
    middleware::bap::activity_message::scriptable_auth::Type68Preset preset{
        .nameHash = nameHash, .elementIndex = elementIndex, .state = state, .visible = visible};
    std::array<std::byte, middleware::bap::activity_message::scriptable_auth::kType68ByteCount>
        body{};
    std::size_t written = 0;
    if (!middleware::bap::activity_message::scriptable_auth::encode_type68(preset, body, written)
        || written != body.size()) {
        return SceneStatus::invalidSlot;
    }
    return server::bap::request_activity_sdk_auth_override(
               view.binding,
               prepared.target,
               &prepared.rosterGroup,
               body,
               middleware::bap::activity_message::scriptable_auth::kType68BitCount,
               prepared.effectiveRegion,
               prepared.activityClientGeneration)
               ? SceneStatus::queued
               : SceneStatus::refused;
}

/**
 * Queues one authored dialogue cue against a reservation a script already holds.
 * @param reservation Output slot the caller reserved for this revision.
 * @return `queued` once the cue is staged, or the refusal that stopped it.
 */
[[nodiscard]] SceneStatus
play_dialogue_cue_reserved(const sdk::BoundView& view,
                           std::uint32_t occurrenceRow,
                           std::uint32_t slotRow,
                           std::uint16_t cueIndex,
                           const host::ScriptableOutputReservation& reservation) noexcept {
    PreparedScene prepared{};
    std::uint16_t authoredCueCount = 0;
    const SceneStatus status =
        prepare_dialogue(view, occurrenceRow, slotRow, cueIndex, prepared, authoredCueCount);
    if (status != SceneStatus::ready) {
        return status;
    }
    return server::bap::request_activity_state_local_dialogue_override(
               view.binding,
               prepared.target,
               prepared.rosterGroup,
               cueIndex,
               authoredCueCount,
               prepared.effectiveRegion,
               prepared.activityClientGeneration,
               &reservation)
               ? SceneStatus::queued
               : SceneStatus::refused;
}

/** Checks one exact SDK-linked authored task. */
SceneStatus task_availability(const sdk::BoundView& view,
                              std::uint32_t occurrenceRow,
                              std::uint32_t slotRow) noexcept {
    PreparedScene prepared{};
    return prepare_task(view, occurrenceRow, slotRow, prepared);
}

SceneStatus objective_reset_availability(const sdk::BoundView& view,
                                         std::uint32_t occurrenceRow,
                                         std::uint32_t slotRow) noexcept {
    PreparedScene prepared{};
    return prepare_objective(view, occurrenceRow, slotRow, prepared);
}

/** Clears every objective element of one prepared objective slot. */
SceneStatus reset_objectives(const sdk::BoundView& view,
                             std::uint32_t occurrenceRow,
                             std::uint32_t slotRow) noexcept {
    PreparedScene prepared{};
    const SceneStatus status = prepare_objective(view, occurrenceRow, slotRow, prepared);
    if (status != SceneStatus::ready) {
        return status;
    }
    if (server::bap::request_activity_state_local_objective_reset(
            view.binding,
            prepared.target,
            prepared.rosterGroup,
            prepared.effectiveRegion,
            prepared.activityClientGeneration)) {
        return SceneStatus::queued;
    }
    return SceneStatus::refused;
}

/** Queues one exact objective reset using a caller-owned output reservation. */
[[nodiscard]] SceneStatus
reset_objectives_reserved(const sdk::BoundView& view,
                          std::uint32_t occurrenceRow,
                          std::uint32_t slotRow,
                          const host::ScriptableOutputReservation& reservation) noexcept {
    PreparedScene prepared{};
    const SceneStatus status = prepare_objective(view, occurrenceRow, slotRow, prepared);
    if (status != SceneStatus::ready) {
        return status;
    }
    return server::bap::request_activity_state_local_objective_reset(
               view.binding,
               prepared.target,
               prepared.rosterGroup,
               prepared.effectiveRegion,
               prepared.activityClientGeneration,
               &reservation)
               ? SceneStatus::queued
               : SceneStatus::refused;
}

/** Queues the next generation through the dedicated type-38 encoder. */
SceneStatus activate_task(const sdk::BoundView& view,
                          std::uint32_t occurrenceRow,
                          std::uint32_t slotRow) noexcept {
    PreparedScene prepared{};
    const SceneStatus status = prepare_task(view, occurrenceRow, slotRow, prepared);
    if (status != SceneStatus::ready) {
        return status;
    }
    if (server::bap::request_activity_state_local_task_override(
            view.binding,
            prepared.target,
            prepared.rosterGroup,
            prepared.effectiveRegion,
            prepared.activityClientGeneration)) {
        return SceneStatus::queued;
    }
    return SceneStatus::refused;
}

/**
 * Activates one authored task against a reservation a script already holds.
 * @param reservation Output slot the caller reserved for this revision.
 * @return `queued` once the task override is staged, or the refusal that stopped it.
 */
[[nodiscard]] SceneStatus
activate_task_reserved(const sdk::BoundView& view,
                       std::uint32_t occurrenceRow,
                       std::uint32_t slotRow,
                       const host::ScriptableOutputReservation& reservation) noexcept {
    PreparedScene prepared{};
    const SceneStatus status = prepare_task(view, occurrenceRow, slotRow, prepared);
    if (status != SceneStatus::ready) {
        return status;
    }
    return server::bap::request_activity_state_local_task_override(
               view.binding,
               prepared.target,
               prepared.rosterGroup,
               prepared.effectiveRegion,
               prepared.activityClientGeneration,
               &reservation)
               ? SceneStatus::queued
               : SceneStatus::refused;
}

/** @return Whether one occurrence's sequence slot can be played right now. */
SceneStatus sequence_availability(const sdk::BoundView& view,
                                  std::uint32_t occurrenceRow,
                                  std::uint32_t slotRow) noexcept {
    PreparedScene prepared{};
    return prepare_typed_behavior(view,
                                  occurrenceRow,
                                  slotRow,
                                  sdk::format::kSequenceSlotType,
                                  sdk::format::kSequenceComponentClass,
                                  sdk::format::kSequenceAuthSchema,
                                  false,
                                  prepared);
}

/** Plays one sequence slot of a prepared behavior occurrence. */
SceneStatus play_sequence(const sdk::BoundView& view,
                          std::uint32_t occurrenceRow,
                          std::uint32_t slotRow) noexcept {
    PreparedScene prepared{};
    const SceneStatus status = prepare_typed_behavior(view,
                                                      occurrenceRow,
                                                      slotRow,
                                                      sdk::format::kSequenceSlotType,
                                                      sdk::format::kSequenceComponentClass,
                                                      sdk::format::kSequenceAuthSchema,
                                                      false,
                                                      prepared);
    if (status != SceneStatus::ready) {
        return status;
    }
    if (server::bap::request_activity_state_local_sequence_override(
            view.binding,
            prepared.target,
            prepared.rosterGroup,
            prepared.effectiveRegion,
            prepared.activityClientGeneration)) {
        return SceneStatus::queued;
    }
    return SceneStatus::refused;
}

/** @return Whether one occurrence's cinematic slot can be driven right now. */
SceneStatus cinematic_availability(const sdk::BoundView& view,
                                   std::uint32_t occurrenceRow,
                                   std::uint32_t slotRow) noexcept {
    PreparedScene prepared{};
    return prepare_typed_behavior(view,
                                  occurrenceRow,
                                  slotRow,
                                  sdk::format::kCinematicSlotType,
                                  sdk::format::kCinematicComponentClass,
                                  sdk::format::kCinematicAuthSchema,
                                  false,
                                  prepared);
}

/** Sets one cinematic slot active or inactive on a prepared behavior occurrence. */
SceneStatus set_cinematic_active(const sdk::BoundView& view,
                                 std::uint32_t occurrenceRow,
                                 std::uint32_t slotRow,
                                 bool active) noexcept {
    PreparedScene prepared{};
    const SceneStatus status = prepare_typed_behavior(view,
                                                      occurrenceRow,
                                                      slotRow,
                                                      sdk::format::kCinematicSlotType,
                                                      sdk::format::kCinematicComponentClass,
                                                      sdk::format::kCinematicAuthSchema,
                                                      false,
                                                      prepared);
    if (status != SceneStatus::ready) {
        return status;
    }
    if (server::bap::request_activity_state_local_cinematic_override(
            view.binding,
            prepared.target,
            prepared.rosterGroup,
            active,
            prepared.effectiveRegion,
            prepared.activityClientGeneration)) {
        return SceneStatus::queued;
    }
    return SceneStatus::refused;
}

/** Plays one sequence slot against an already-reserved Host output revision. */
SceneStatus play_sequence_reserved(const sdk::BoundView& view,
                                   std::uint32_t occurrenceRow,
                                   std::uint32_t slotRow,
                                   const host::ScriptableOutputReservation& reservation) noexcept {
    PreparedScene prepared{};
    const SceneStatus status = prepare_typed_behavior(view,
                                                      occurrenceRow,
                                                      slotRow,
                                                      sdk::format::kSequenceSlotType,
                                                      sdk::format::kSequenceComponentClass,
                                                      sdk::format::kSequenceAuthSchema,
                                                      false,
                                                      prepared);
    if (status != SceneStatus::ready) {
        return status;
    }
    return server::bap::request_activity_state_local_sequence_override(
               view.binding,
               prepared.target,
               prepared.rosterGroup,
               prepared.effectiveRegion,
               prepared.activityClientGeneration,
               &reservation)
               ? SceneStatus::queued
               : SceneStatus::refused;
}

/**
 * Drives one cinematic behavior against a reservation a script already holds.
 * @param reservation Output slot the caller reserved for this revision.
 * @return `queued` once the override is staged, or the refusal that stopped it.
 */
SceneStatus
set_cinematic_active_reserved(const sdk::BoundView& view,
                              std::uint32_t occurrenceRow,
                              std::uint32_t slotRow,
                              bool active,
                              const host::ScriptableOutputReservation& reservation) noexcept {
    PreparedScene prepared{};
    const SceneStatus status = prepare_typed_behavior(view,
                                                      occurrenceRow,
                                                      slotRow,
                                                      sdk::format::kCinematicSlotType,
                                                      sdk::format::kCinematicComponentClass,
                                                      sdk::format::kCinematicAuthSchema,
                                                      false,
                                                      prepared);
    if (status != SceneStatus::ready) {
        return status;
    }
    return server::bap::request_activity_state_local_cinematic_override(
               view.binding,
               prepared.target,
               prepared.rosterGroup,
               active,
               prepared.effectiveRegion,
               prepared.activityClientGeneration,
               &reservation)
               ? SceneStatus::queued
               : SceneStatus::refused;
}

/** Finds the occurrence a slot currently belongs to. @return `invalidView` on a stale view. */
[[nodiscard]] static SceneStatus current_behavior_occurrence(
    const sdk::BoundView& view, std::uint32_t slotRow, std::uint32_t& occurrenceRow) noexcept {
    occurrenceRow = sdk::format::kAbsentIndex;
    Snapshot snapshot{};
    if (query(view, snapshot) != Status::ready || view.catalog == nullptr) {
        return SceneStatus::invalidView;
    }
    const auto slots = view.catalog->slots();
    const auto occurrences = view.catalog->occurrences();
    if (slotRow >= slots.size()) {
        return SceneStatus::invalidSlot;
    }
    server::bap::ActivityLinkView link{};
    const SceneStatus live = scene_binding_status(view, link);
    if (live != SceneStatus::ready) {
        return live;
    }
    const auto selected = behavior_scope::select(occurrences,
                                                 view.catalog->states(),
                                                 view.catalog->bubbles(),
                                                 view.scenarioRow,
                                                 slots[slotRow].objectIndex,
                                                 snapshot.plan.stateRow,
                                                 link.effectiveRegion);
    if (selected.ambiguous) {
        return SceneStatus::ambiguousTarget;
    }
    occurrenceRow = selected.row;
    return occurrenceRow == sdk::format::kAbsentIndex ? SceneStatus::targetUnavailable
                                                      : SceneStatus::ready;
}

SceneStatus sequence_slot_availability(const sdk::BoundView& view, std::uint32_t slotRow) noexcept {
    std::uint32_t occurrenceRow = 0;
    const SceneStatus found = current_behavior_occurrence(view, slotRow, occurrenceRow);
    return found == SceneStatus::ready ? sequence_availability(view, occurrenceRow, slotRow)
                                       : found;
}

SceneStatus objective_reset_slot_availability(const sdk::BoundView& view,
                                              std::uint32_t slotRow) noexcept {
    std::uint32_t occurrenceRow = 0;
    const SceneStatus found = current_behavior_occurrence(view, slotRow, occurrenceRow);
    return found == SceneStatus::ready ? objective_reset_availability(view, occurrenceRow, slotRow)
                                       : found;
}

SceneStatus
reset_objectives_slot_reserved(const sdk::BoundView& view,
                               std::uint32_t slotRow,
                               const host::ScriptableOutputReservation& reservation) noexcept {
    std::uint32_t occurrenceRow = 0;
    const SceneStatus found = current_behavior_occurrence(view, slotRow, occurrenceRow);
    return found == SceneStatus::ready
               ? reset_objectives_reserved(view, occurrenceRow, slotRow, reservation)
               : found;
}

SceneStatus
play_sequence_slot_reserved(const sdk::BoundView& view,
                            std::uint32_t slotRow,
                            const host::ScriptableOutputReservation& reservation) noexcept {
    std::uint32_t occurrenceRow = 0;
    const SceneStatus found = current_behavior_occurrence(view, slotRow, occurrenceRow);
    return found == SceneStatus::ready
               ? play_sequence_reserved(view, occurrenceRow, slotRow, reservation)
               : found;
}

SceneStatus cinematic_slot_availability(const sdk::BoundView& view,
                                        std::uint32_t slotRow) noexcept {
    std::uint32_t occurrenceRow = 0;
    const SceneStatus found = current_behavior_occurrence(view, slotRow, occurrenceRow);
    return found == SceneStatus::ready ? cinematic_availability(view, occurrenceRow, slotRow)
                                       : found;
}

/**
 * Resolves the slot's current occurrence, then drives its cinematic behavior.
 * @param reservation Output slot the caller reserved for this revision.
 * @return `queued` once the override is staged, or the refusal that stopped it.
 */
SceneStatus
set_cinematic_slot_active_reserved(const sdk::BoundView& view,
                                   std::uint32_t slotRow,
                                   bool active,
                                   const host::ScriptableOutputReservation& reservation) noexcept {
    std::uint32_t occurrenceRow = 0;
    const SceneStatus found = current_behavior_occurrence(view, slotRow, occurrenceRow);
    return found == SceneStatus::ready
               ? set_cinematic_active_reserved(view, occurrenceRow, slotRow, active, reservation)
               : found;
}

/** Starts one named state of the actor a type-42 sensor drives, behind its rising generation. */
[[nodiscard]] static SceneStatus
play_performance_reserved(const sdk::BoundView& view,
                          std::uint32_t occurrenceRow,
                          std::uint32_t slotRow,
                          std::uint32_t stateNameHash,
                          const host::ScriptableOutputReservation& reservation) noexcept {
    PreparedScene prepared{};
    const SceneStatus status = prepare_typed_behavior(view,
                                                      occurrenceRow,
                                                      slotRow,
                                                      sdk::format::kPerformanceSlotType,
                                                      sdk::format::kPerformanceComponentClass,
                                                      sdk::format::kPerformanceAuthSchema,
                                                      false,
                                                      prepared);
    if (status != SceneStatus::ready) {
        return status;
    }
    return server::bap::request_activity_state_local_performance_override(
               view.binding,
               prepared.target,
               prepared.rosterGroup,
               stateNameHash,
               prepared.effectiveRegion,
               prepared.activityClientGeneration,
               &reservation)
               ? SceneStatus::queued
               : SceneStatus::refused;
}

/** Starts one state on the squad the sensor drives, through the slot's current occurrence. */
SceneStatus
play_performance_slot_reserved(const sdk::BoundView& view,
                               std::uint32_t slotRow,
                               std::uint32_t stateNameHash,
                               const host::ScriptableOutputReservation& reservation) noexcept {
    std::uint32_t occurrenceRow = 0;
    const SceneStatus found = current_behavior_occurrence(view, slotRow, occurrenceRow);
    return found == SceneStatus::ready
               ? play_performance_reserved(view, occurrenceRow, slotRow, stateNameHash, reservation)
               : found;
}

/** Starts one authored state on the squad a type-42 sensor drives, for an operator action. */
SceneStatus play_performance_slot(const sdk::BoundView& view,
                                  std::uint32_t slotRow,
                                  std::uint32_t stateNameHash) noexcept {
    std::uint32_t occurrenceRow = 0;
    const SceneStatus found = current_behavior_occurrence(view, slotRow, occurrenceRow);
    if (found != SceneStatus::ready) {
        return found;
    }
    PreparedScene prepared{};
    const SceneStatus status = prepare_typed_behavior(view,
                                                      occurrenceRow,
                                                      slotRow,
                                                      sdk::format::kPerformanceSlotType,
                                                      sdk::format::kPerformanceComponentClass,
                                                      sdk::format::kPerformanceAuthSchema,
                                                      false,
                                                      prepared);
    if (status != SceneStatus::ready) {
        return status;
    }
    return server::bap::request_activity_state_local_performance_override(
               view.binding,
               prepared.target,
               prepared.rosterGroup,
               stateNameHash,
               prepared.effectiveRegion,
               prepared.activityClientGeneration)
               ? SceneStatus::queued
               : SceneStatus::refused;
}

SceneStatus task_slot_availability(const sdk::BoundView& view, std::uint32_t slotRow) noexcept {
    std::uint32_t occurrenceRow = 0;
    const SceneStatus found = current_behavior_occurrence(view, slotRow, occurrenceRow);
    return found == SceneStatus::ready ? task_availability(view, occurrenceRow, slotRow) : found;
}

SceneStatus
activate_task_slot_reserved(const sdk::BoundView& view,
                            std::uint32_t slotRow,
                            const host::ScriptableOutputReservation& reservation) noexcept {
    std::uint32_t occurrenceRow = 0;
    const SceneStatus found = current_behavior_occurrence(view, slotRow, occurrenceRow);
    return found == SceneStatus::ready
               ? activate_task_reserved(view, occurrenceRow, slotRow, reservation)
               : found;
}

SceneStatus dialogue_cue_slot_availability(const sdk::BoundView& view,
                                           std::uint32_t slotRow,
                                           std::uint16_t cueIndex) noexcept {
    std::uint32_t occurrenceRow = 0;
    const SceneStatus found = current_behavior_occurrence(view, slotRow, occurrenceRow);
    return found == SceneStatus::ready
               ? dialogue_cue_availability(view, occurrenceRow, slotRow, cueIndex)
               : found;
}

/**
 * Resolves the slot's current occurrence, then queues its authored dialogue cue.
 * @param reservation Output slot the caller reserved for this revision.
 * @return `queued` once the cue is staged, or the refusal that stopped it.
 */
SceneStatus
play_dialogue_cue_slot_reserved(const sdk::BoundView& view,
                                std::uint32_t slotRow,
                                std::uint16_t cueIndex,
                                const host::ScriptableOutputReservation& reservation) noexcept {
    std::uint32_t occurrenceRow = 0;
    const SceneStatus found = current_behavior_occurrence(view, slotRow, occurrenceRow);
    return found == SceneStatus::ready
               ? play_dialogue_cue_reserved(view, occurrenceRow, slotRow, cueIndex, reservation)
               : found;
}

/** Queues one preflighted authored scene only through an exact unarmed Host revision. */
SceneStatus activate_authored_scene_reserved(const sdk::BoundView& view,
                                             std::uint32_t occurrenceRow,
                                             std::uint32_t slotRow,
                                             const host::ScriptableOutputReservation& reservation,
                                             std::uint32_t eventKey,
                                             bool stop) noexcept {
    PreparedScene prepared{};
    const SceneStatus status = prepare_scene(view, occurrenceRow, slotRow, prepared);
    if (status != SceneStatus::ready) {
        return status;
    }
    if (server::bap::request_activity_state_local_authored_scene_override(
            view.binding,
            prepared.target,
            prepared.rosterGroup,
            prepared.effectiveRegion,
            prepared.activityClientGeneration,
            &reservation,
            prepared.sceneDependencies,
            eventKey,
            stop)) {
        return SceneStatus::queued;
    }
    return SceneStatus::refused;
}

/** Returns stable concise text for one mission runtime result. */
const char* status_name(Status status) noexcept {
    switch (status) {
    case Status::ready:
        return "ready";
    case Status::invalidView:
        return "invalid_view";
    case Status::staleBinding:
        return "stale_binding";
    case Status::staleActivityClient:
        return "stale_activity_client";
    case Status::noActivityLink:
        return "no_activity_link";
    case Status::missingLiveSliceSet:
        return "missing_live_slice_set";
    case Status::wrongScenario:
        return "wrong_scenario";
    case Status::wrongSliceSet:
        return "wrong_slice_set";
    case Status::missingInitialState:
        return "missing_initial_state";
    case Status::ambiguousInitialState:
        return "ambiguous_initial_state";
    case Status::invalidOccurrence:
        return "invalid_occurrence";
    case Status::schemaJoinNotExact:
        return "schema_join_not_exact";
    case Status::invalidRosterGroup:
        return "invalid_roster_group";
    case Status::rosterKeyConflict:
        return "roster_key_conflict";
    case Status::groupCapacityExceeded:
        return "group_capacity_exceeded";
    case Status::outputBusy:
        return "output_busy";
    case Status::refused:
        return "refused";
    }
    return "refused";
}

/** Returns stable concise text for one authored-scene result. */
const char* status_name(SceneStatus status) noexcept {
    switch (status) {
    case SceneStatus::ready:
        return "ready";
    case SceneStatus::queued:
        return "queued";
    case SceneStatus::invalidView:
        return "invalid_view";
    case SceneStatus::staleBinding:
        return "stale_binding";
    case SceneStatus::staleActivityClient:
        return "stale_activity_client";
    case SceneStatus::noActivityLink:
        return "no_activity_link";
    case SceneStatus::invalidOccurrence:
        return "invalid_occurrence";
    case SceneStatus::invalidSlot:
        return "invalid_slot";
    case SceneStatus::wrongScenario:
        return "wrong_scenario";
    case SceneStatus::wrongState:
        return "wrong_state";
    case SceneStatus::schemaJoinNotExact:
        return "schema_join_not_exact";
    case SceneStatus::missingResource:
        return "missing_resource";
    case SceneStatus::ambiguousResource:
        return "ambiguous_resource";
    case SceneStatus::targetUnavailable:
        return "target_unavailable";
    case SceneStatus::ambiguousTarget:
        return "ambiguous_target";
    case SceneStatus::missionSeedUnavailable:
        return "mission_seed_unavailable";
    case SceneStatus::missionSeedPending:
        return "mission_seed_pending";
    case SceneStatus::outputBusy:
        return "output_busy";
    case SceneStatus::refused:
        return "refused";
    }
    return "refused";
}

} // namespace sunrise::server::activity::activity_sdk_mission
