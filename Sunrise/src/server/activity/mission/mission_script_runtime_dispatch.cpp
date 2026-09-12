#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <string_view>

#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "../../../state/activity/membership/activity_membership_query.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/build_data/spawn_sets/spawn_set_catalog.h"
#include "../../gameplay/squad_entity_retirement.h"
#include "../activity_sdk_device_runtime.h"
#include "../activity_sdk_lifetime_runtime.h"
#include "../activity_sdk_mission_runtime.h"
#include "../activity_sdk_squad_runtime.h"
#include "mission_script_runtime.h"
#include "mission_script_runtime_internal.h"

// The intent fan-out reserves one Host output revision, then asks one typed adapter to encode it.

namespace sunrise::server::activity::mission {
namespace {

namespace devices = activity_sdk_devices;
namespace lifetimes = activity_sdk_lifetime;
namespace scenes = activity_sdk_mission;
namespace squads = activity_sdk_squads;
namespace scriptable_auth = middleware::bap::activity_message::scriptable_auth;
namespace squad_auth = middleware::bap::activity_message::squad_auth;

const void* g_actorCommandPolicyContext{};
ActorCommandPolicy g_actorCommandPolicy{};

/** Resolves one durable selector against the current pinned SDK. */
[[nodiscard]] ActorCommandPolicyStatus
dispatch_actor_command(const RuntimeInstance& instance, const lua_vm::Intent& intent) noexcept {
    if (instance.view.catalog == nullptr) {
        return ActorCommandPolicyStatus::refused;
    }
    const sdk::Snapshot published = sdk::snapshot();
    if (published == nullptr) {
        return ActorCommandPolicyStatus::refused;
    }
    const std::span<const std::byte> sdkBuild = published->sdk_build_sha256();
    if (sdkBuild.size() != intent.sdkBuildSha256.size()
        || !std::equal(sdkBuild.begin(), sdkBuild.end(), intent.sdkBuildSha256.begin())
        || instance.view.catalog->sdk_build_sha256().size() != sdkBuild.size()
        || !std::equal(
            sdkBuild.begin(), sdkBuild.end(), instance.view.catalog->sdk_build_sha256().begin())) {
        return ActorCommandPolicyStatus::refused;
    }
    const auto squads = published->squads();
    if (intent.firstRow >= squads.size()
        || (squads[intent.firstRow].flags & format::kSquadRunnableMask)
               != format::kSquadRunnableMask) {
        return ActorCommandPolicyStatus::refused;
    }
    const auto commands = published->actor_command_definitions();
    for (std::size_t index = 0; index < commands.size(); ++index) {
        const format::ActorCommandDefinition& command = commands[index];
        if (command.selector != intent.actorCommandSelector) {
            continue;
        }
        const bool valueValid = command.effect == format::ActorCommandEffect::setFaction
                                && (intent.actorCommandValue == command.factionNone
                                    || intent.actorCommandValue == command.factionRemoved
                                    || intent.actorCommandValue == command.factionHostileToAll);
        if (command.payloadHandle == 0
            || command.provenance != format::ActorSemanticProvenance::executableStatic
            || command.flags != format::kActorCommandDefinitionExact || !valueValid) {
            return ActorCommandPolicyStatus::refused;
        }
        if (g_actorCommandPolicy == nullptr) {
            return ActorCommandPolicyStatus::unavailable;
        }
        const ActorCommandPolicyRequest request{
            .binding = instance.view.binding,
            .sdkBuildSha256 = intent.sdkBuildSha256,
            .squadRow = intent.firstRow,
            .commandRow = static_cast<std::uint32_t>(index),
            .commandSelector = command.selector,
            .value = intent.actorCommandValue,
        };
        return g_actorCommandPolicy(g_actorCommandPolicyContext, request);
    }
    return ActorCommandPolicyStatus::refused;
}

/** Records the one deterministic execution attempt for diagnostics. */
void begin_intent_attempt(RuntimeInstance& instance, std::uint64_t now) noexcept {
    if (instance.intentAttempts == 0) {
        instance.firstIntentAttempt = now;
    }
    ++instance.intentAttempts;
}

/** Assigns one exact Host output revision without clearing the durable intent. */
[[nodiscard]] bool assign_delivery_state(RuntimeInstance& instance,
                                         std::uint64_t hostOutputRevision) noexcept {
    mission_state::Snapshot snapshot{};
    const mission_state::Status status =
        mission_state::assign_intent_output(instance.view.binding,
                                            instance.programKey,
                                            instance.missionStateRevision,
                                            instance.durableIntentSequence,
                                            hostOutputRevision,
                                            snapshot);
    if (status != mission_state::Status::ready) {
        lua_vm::fault(instance.vm, "durable mission intent assignment compare was refused");
        instance.programStatus = ProgramStatus::programError;
        log_line(
            core::log::Level::warn, &instance, "intent_assign", mission_state::status_name(status));
        return false;
    }
    accept_mission_state(instance, snapshot);
    return instance.durableHostOutputRevision == hostOutputRevision;
}

/** Reserves one exact Host revision, then makes the durable head own it before enqueue. */
[[nodiscard]] bool reserve_delivery(RuntimeInstance& instance,
                                    host::ScriptableOutputReservation& reservation) noexcept {
    if (!host::reserve_scriptable_output(
            instance.view.binding, reservation, instance.durableIntentSequence)) {
        return false;
    }
    if (!assign_delivery_state(instance, reservation.revision)) {
        if (!host::release_scriptable_output(reservation)) {
            log_line(core::log::Level::warn, &instance, "intent_reserve", "release_refused");
        }
        persist_mission_fault(instance);
        clear_pending_events(instance.view.binding);
        clear_delivery(instance);
        return false;
    }
    instance.expectedScriptableRevision = reservation.revision;
    return true;
}

/** Releases State before opening the reserved Host lane after an adapter refusal. */
[[nodiscard]] bool
abandon_reserved_delivery(RuntimeInstance& instance,
                          const host::ScriptableOutputReservation& reservation) noexcept {
    if (!release_delivery_state(instance)) {
        // Keep the Host reservation fail-closed when State cannot remove its ownership.
        persist_mission_fault(instance);
        clear_pending_events(instance.view.binding);
        clear_delivery(instance);
        return false;
    }
    if (!host::release_scriptable_output(reservation)) {
        fault_instance(instance, "Host mission output reservation release was refused");
        instance.programStatus = ProgramStatus::programError;
        log_line(core::log::Level::warn, &instance, "intent_reserve", "release_refused");
        clear_pending_events(instance.view.binding);
        clear_delivery(instance);
        return false;
    }
    return true;
}

/**
 * Enters the first delivery stage once an adapter has queued the request.
 * @param now Service tick the commit deadline is measured from.
 * @param result Diagnostic name of the queued request.
 */
void await_host_commit(RuntimeInstance& instance,
                       std::uint64_t now,
                       std::string_view result) noexcept {
    if (instance.expectedScriptableRevision == 0
        || instance.durableHostOutputRevision != instance.expectedScriptableRevision) {
        fault_delivery(instance,
                       "revision_unavailable",
                       "queued intent did not match durable Host revision ownership");
        return;
    }
    instance.lastIntentStatus = (std::numeric_limits<std::uint16_t>::max)();
    instance.deliveryStage = DeliveryStage::awaitingHostCommit;
    instance.deliveryDeadline = deadline_after(now, kHostCommitTimeoutMs);
    log_line(core::log::Level::info, &instance, "intent", result);
}

} // namespace

/** Installs the gameplay policy seam. */
void install_actor_command_policy(const void* context, ActorCommandPolicy policy) noexcept {
    g_actorCommandPolicyContext = context;
    g_actorCommandPolicy = policy;
}

/**
 * Moves the client to the region a freshly selected mission state belongs to.
 * The client picks its object registry from the loaded slice-set entry, so a state in another
 * region has no findable objects until it transitions. Message 12 is the only mid-activity move.
 * @param instance Runtime instance whose state selection just published.
 * @param plan Published plan naming the target region and its bubble.
 */
void arm_state_region_teleport(RuntimeInstance& instance,
                               const server::bap::ActivityMissionSeedPlan& plan) noexcept {
    namespace membership = ::sunrise::state::activity::membership;
    const auto& destination = instance.view.binding.destination;
    if (destination.packageNameLength == 0
        || destination.packageNameLength > destination.packageName.size()
        || plan.effectiveRegion > static_cast<std::uint32_t>(membership::kMaximumSliceSetIndex)) {
        return;
    }
    const std::int32_t reported = membership::player_region(instance.view.binding.sessionId);
    // A sibling-state transition still needs the host teleport to order the spawn.
    if (reported == static_cast<std::int32_t>(plan.effectiveRegion)) {
        // Already there. Clear any earlier arm so the mirror owns the block again.
        static_cast<void>(membership::arm_host_teleport(
            instance.view.binding.sessionId, membership::kAbsentSliceSetIndex, 0));
        return;
    }
    // Each alternate scenario entry is its own packed region, so a sibling state still travels.
    const std::string_view name(reinterpret_cast<const char*>(destination.packageName.data()),
                                destination.packageNameLength);
    ::sunrise::state::build_data::scenarios::Definition layout{};
    if (!::sunrise::state::build_data::find_scenario_layout(name, layout)
        || plan.bubbleOrdinal >= layout.bubbleHashes.size()) {
        return;
    }
    const std::uint32_t hash = layout.bubbleHashes[plan.bubbleOrdinal];
    if (hash == 0) {
        // Without the slice-set name hash the client cannot resolve the target, and a zero would
        // arm a move it can never finish.
        return;
    }
    const bool armed = membership::arm_host_teleport(
        instance.view.binding.sessionId, static_cast<std::int32_t>(plan.effectiveRegion), hash);
    log_line(core::log::Level::info,
             &instance,
             "state_region",
             armed ? "teleport_armed" : "teleport_unchanged");
}

/**
 * @return True while a scene-family request is valid but its mission-seed lease has not published.
 * The request stays queued: refusing it drops an answer the client is still owed.
 */
[[nodiscard]] bool scene_lease_still_publishing(scenes::SceneStatus status) noexcept {
    return status == scenes::SceneStatus::missionSeedPending
           || status == scenes::SceneStatus::outputBusy;
}

/** Raises one queued intent, or advances the delivery already in flight. */
void dispatch_intent(RuntimeInstance& instance, std::uint64_t now) noexcept {
    if (instance.programStatus != ProgramStatus::loaded) {
        return;
    }
    lua_vm::Intent intent{};
    if (!lua_vm::pending_intent(instance.vm, intent)) {
        if (instance.deliveryStage != DeliveryStage::idle) {
            fault_delivery(instance, "missing_intent", "delivery lost its pending Lua intent");
        } else {
            clear_delivery(instance);
        }
        return;
    }
    if (intent_lifetime_expired(instance, now)) {
        if (reconcile_expired_delivery(instance)) {
            return;
        }
        refuse_delivery(instance,
                        "intent_timeout",
                        "intent exceeded its delivery lifetime",
                        host::EffectOutcome::expired);
        return;
    }
    if (instance.deliveryStage != DeliveryStage::idle) {
        if (reconcile_transport_stage(instance)) {
            return;
        }
        static_cast<void>(service_delivery_timeout(instance, now));
        return;
    }
    begin_intent_attempt(instance, now);
    switch (intent.kind) {
    case lua_vm::IntentKind::selectMissionState: {
        if (intent.retirePlacedProps) {
            namespace retirement = server::gameplay::squad_entity_retirement;
            const auto* world = instance.worldView.snapshot();
            const auto placement =
                state::activity::membership::reported_placement(instance.view.binding.sessionId);
            const auto status =
                world == nullptr || instance.publicTarget
                    ? retirement::TransitionStatus::refused
                    : retirement::begin_placed_transition(
                          instance.view,
                          *world,
                          intent.requestKey,
                          state::activity::membership::instantiated_region(placement),
                          intent.effectiveRegion);
            if (status == retirement::TransitionStatus::pending) {
                report_intent_status(
                    instance, kIntentStatusStateTransitionPending, "placed_retirement_pending");
                return;
            }
            if (status != retirement::TransitionStatus::ready) {
                refuse_delivery(instance,
                                "state_refused",
                                "placed_lifetimes_unavailable",
                                host::EffectOutcome::refused);
                return;
            }
        }
        scenes::Snapshot selected{};
        const scenes::Status status =
            scenes::select_state(instance.view,
                                 intent.effectiveRegion,
                                 std::span(intent.seedOmissions).first(intent.seedOmissionCount),
                                 selected);
        if (status == scenes::Status::outputBusy) {
            report_intent_status(
                instance, kIntentStatusSceneOutputBusy, scenes::status_name(status));
            return;
        }
        if (status != scenes::Status::ready || !selected.configured
            || selected.plan.effectiveRegion
                   != static_cast<std::uint32_t>(intent.effectiveRegion)) {
            refuse_delivery(instance,
                            "state_refused",
                            scenes::status_name(status),
                            host::EffectOutcome::refused);
            return;
        }
        // The selected effective region is an authored-state key, not a region the client reports.
        // Publishing this lease revision is the completion edge, except when publication waits for
        // arrival: there the teleport is armed first, because arrival closes that window.
        if (!selected.regionArrivalPending
            && (selected.publicationPending || selected.revision == 0
                || selected.publishedRevision != selected.revision)) {
            report_intent_status(
                instance, kIntentStatusStateTransitionPending, "state_transition_pending");
            return;
        }
        // A selected state names its own slice-set region. Until the client transitions there its
        // object registry comes from the loaded slice-set entry, so the new state's objects stay
        // unfindable. Arming the host teleport is the only mid-activity move.
        arm_state_region_teleport(instance, selected.plan);
        static_cast<void>(complete_local_effect(instance, "state_selected"));
        return;
    }
    case lua_vm::IntentKind::restartCheckpoint: {
        if (intent.checkpointReleaseRequest != 0) {
            if (::sunrise::state::activity::membership::release_hard_wipe(
                    instance.view.binding, intent.checkpointReleaseRequest)) {
                static_cast<void>(complete_local_effect(instance, "checkpoint_reset_ready"));
            } else {
                refuse_delivery(instance,
                                "checkpoint_refused",
                                "wipe_not_active",
                                host::EffectOutcome::refused);
            }
            return;
        }
        namespace data = ::sunrise::state::build_data;
        const auto& destination = instance.view.binding.destination;
        const std::string_view package(
            reinterpret_cast<const char*>(destination.packageName.data()),
            destination.packageNameLength);
        data::scenarios::Definition layout{};
        data::spawn_sets::NameHash spawn{};
        // A wipe restarts the whole party at an authored spawn set of the region they are in.
        if (instance.publicTarget || !instance.lastFireteamLife.all_dead()
            || instance.activeRegion != intent.effectiveRegion
            || !data::find_scenario_layout(package, layout)
            || !data::spawn_sets::find_hash({layout.spawnStem.data(), layout.spawnStemLength},
                                            intent.checkpointSpawnHash,
                                            spawn)
            || spawn.pointCount == 0) {
            refuse_delivery(instance,
                            "checkpoint_refused",
                            "party_or_spawn_unavailable",
                            host::EffectOutcome::refused);
            return;
        }
        if (::sunrise::state::activity::membership::arm_hard_wipe(instance.view.binding,
                                                                  intent.requestKey,
                                                                  intent.effectiveRegion,
                                                                  intent.checkpointSpawnHash)) {
            static_cast<void>(complete_local_effect(instance, "checkpoint_wipe_armed"));
        } else {
            refuse_delivery(
                instance, "checkpoint_refused", "membership_busy", host::EffectOutcome::refused);
        }
        return;
    }
    case lua_vm::IntentKind::placeSquad: {
        const std::span<const std::int32_t> counts(intent.squadCounts.data(), intent.squadCount);
        const auto mode = static_cast<squad_auth::Mode>(intent.squadMode);
        host::ScriptableOutputReservation reservation{};
        if (!reserve_delivery(instance, reservation)) {
            if (instance.programStatus == ProgramStatus::loaded) {
                refuse_delivery(instance,
                                "squad_refused",
                                "host_reservation_unavailable",
                                host::EffectOutcome::refused);
            }
            return;
        }
        const squads::Status status = squads::place_reserved(instance.view,
                                                             intent.firstRow,
                                                             counts,
                                                             mode,
                                                             reservation,
                                                             std::nullopt,
                                                             intent.squadRetireOnReturn);
        if (status == squads::Status::queued) {
            await_host_commit(instance, now, "squad_enqueued");
        } else if (!abandon_reserved_delivery(instance, reservation)) {
            return;
        } else {
            refuse_delivery(instance,
                            "squad_refused",
                            squads::status_name(status),
                            host::EffectOutcome::refused);
        }
        return;
    }
    case lua_vm::IntentKind::actorCommand: {
        const ActorCommandPolicyStatus status = dispatch_actor_command(instance, intent);
        if (status == ActorCommandPolicyStatus::queued) {
            static_cast<void>(complete_local_effect(instance, "actor_command_queued"));
        } else {
            refuse_delivery(instance,
                            "actor_command_refused",
                            status == ActorCommandPolicyStatus::unavailable
                                ? "gameplay_policy_unavailable"
                                : "sdk_command_refused",
                            host::EffectOutcome::refused);
        }
        return;
    }
    case lua_vm::IntentKind::bindCombatantToSquad: {
        host::ScriptableOutputReservation reservation{};
        if (!reserve_delivery(instance, reservation)) {
            if (instance.programStatus == ProgramStatus::loaded) {
                refuse_delivery(instance,
                                "combatant_binding_refused",
                                "host_reservation_unavailable",
                                host::EffectOutcome::refused);
            }
            return;
        }
        const devices::Status status =
            devices::bind_combatant_to_squad_reserved(instance.view, intent.firstRow, reservation);
        if (status == devices::Status::queued) {
            await_host_commit(instance, now, "combatant_binding_enqueued");
        } else if (!abandon_reserved_delivery(instance, reservation)) {
            return;
        } else {
            refuse_delivery(instance,
                            "combatant_binding_refused",
                            devices::status_name(status),
                            host::EffectOutcome::refused);
        }
        return;
    }
    case lua_vm::IntentKind::setObjectActive: {
        host::ScriptableOutputReservation reservation{};
        if (!reserve_delivery(instance, reservation)) {
            if (instance.programStatus == ProgramStatus::loaded) {
                refuse_delivery(instance,
                                "object_refused",
                                "host_reservation_unavailable",
                                host::EffectOutcome::refused);
            }
            return;
        }
        const devices::Status status = devices::set_objects_active_reserved(
            instance.view,
            intent.firstRow,
            std::span(intent.burstRows).first(intent.burstRowCount),
            intent.entryIndex,
            intent.active,
            reservation);
        if (status == devices::Status::queued) {
            await_host_commit(instance, now, "object_enqueued");
        } else if (!abandon_reserved_delivery(instance, reservation)) {
            return;
        } else {
            refuse_delivery(instance,
                            "object_refused",
                            devices::status_name(status),
                            host::EffectOutcome::refused);
        }
        return;
    }
    case lua_vm::IntentKind::setDeviceChannel: {
        const auto channel = static_cast<scriptable_auth::Type23Channel>(intent.deviceChannel);
        host::ScriptableOutputReservation reservation{};
        if (!reserve_delivery(instance, reservation)) {
            if (instance.programStatus == ProgramStatus::loaded) {
                refuse_delivery(instance,
                                "device_refused",
                                "host_reservation_unavailable",
                                host::EffectOutcome::refused);
            }
            return;
        }
        const devices::Status status = devices::set_channel_reserved(instance.view,
                                                                     intent.firstRow,
                                                                     channel,
                                                                     intent.deviceValue,
                                                                     intent.deviceSnap,
                                                                     reservation);
        if (status == devices::Status::queued) {
            await_host_commit(instance, now, "device_enqueued");
        } else if (!abandon_reserved_delivery(instance, reservation)) {
            return;
        } else {
            refuse_delivery(instance,
                            "device_refused",
                            devices::status_name(status),
                            host::EffectOutcome::refused);
        }
        return;
    }
    case lua_vm::IntentKind::fireTrigger: {
        host::ScriptableOutputReservation reservation{};
        if (!reserve_delivery(instance, reservation)) {
            if (instance.programStatus == ProgramStatus::loaded) {
                refuse_delivery(instance,
                                "trigger_refused",
                                "host_reservation_unavailable",
                                host::EffectOutcome::refused);
            }
            return;
        }
        const devices::Status status =
            devices::fire_trigger_reserved(instance.view, intent.firstRow, reservation);
        if (status == devices::Status::queued) {
            await_host_commit(instance, now, "trigger_enqueued");
        } else if (!abandon_reserved_delivery(instance, reservation)) {
            return;
        } else {
            refuse_delivery(instance,
                            "trigger_refused",
                            devices::status_name(status),
                            host::EffectOutcome::refused);
        }
        return;
    }
    case lua_vm::IntentKind::playSequence:
    case lua_vm::IntentKind::setCinematicActive: {
        const bool sequence = intent.kind == lua_vm::IntentKind::playSequence;
        host::ScriptableOutputReservation reservation{};
        if (!reserve_delivery(instance, reservation)) {
            if (instance.programStatus == ProgramStatus::loaded) {
                refuse_delivery(instance,
                                sequence ? "sequence_refused" : "cinematic_refused",
                                "host_reservation_unavailable",
                                host::EffectOutcome::refused);
            }
            return;
        }
        const scenes::SceneStatus status =
            sequence
                ? scenes::play_sequence_slot_reserved(instance.view, intent.firstRow, reservation)
                : scenes::set_cinematic_slot_active_reserved(
                      instance.view, intent.firstRow, intent.active, reservation);
        if (status == scenes::SceneStatus::queued) {
            await_host_commit(instance, now, sequence ? "sequence_enqueued" : "cinematic_enqueued");
        } else if (!abandon_reserved_delivery(instance, reservation)) {
            return;
        } else if (scene_lease_still_publishing(status)) {
            // The lease numbers say which side moved. The status dedup reports them once.
            std::array<char, 176> detail{};
            scenes::Snapshot lease{};
            const scenes::Status leaseStatus = scenes::query(instance.view, lease);
            const int written =
                std::snprintf(detail.data(),
                              detail.size(),
                              "%s lease=%s configured=%d revision=%llu published=%llu "
                              "pending=%d arrival=%d region=%d plan_region=%u state_row=%u",
                              scenes::status_name(status),
                              scenes::status_name(leaseStatus),
                              lease.configured ? 1 : 0,
                              static_cast<unsigned long long>(lease.revision),
                              static_cast<unsigned long long>(lease.publishedRevision),
                              lease.publicationPending ? 1 : 0,
                              lease.regionArrivalPending ? 1 : 0,
                              lease.effectiveRegion,
                              static_cast<unsigned>(lease.plan.effectiveRegion),
                              static_cast<unsigned>(lease.plan.stateRow));
            report_intent_status(
                instance,
                kIntentStatusSceneLeasePending,
                written > 0 ? std::string_view{detail.data(), static_cast<std::size_t>(written)}
                            : scenes::status_name(status));
        } else {
            refuse_delivery(instance,
                            sequence ? "sequence_refused" : "cinematic_refused",
                            scenes::status_name(status),
                            host::EffectOutcome::refused);
        }
        return;
    }
    case lua_vm::IntentKind::playActorSequence: {
        host::ScriptableOutputReservation reservation{};
        if (!reserve_delivery(instance, reservation)) {
            if (instance.programStatus == ProgramStatus::loaded) {
                refuse_delivery(instance,
                                "actor_sequence_refused",
                                "host_reservation_unavailable",
                                host::EffectOutcome::refused);
            }
            return;
        }
        const devices::Status status =
            intent.firstRow == intent.sequenceOwner.slotRow
                ? devices::play_combatant_sequence_reserved(
                      instance.view, intent.sequenceOwner, intent.secondRow, reservation)
                : devices::Status::invalidSlot;
        if (status == devices::Status::queued) {
            await_host_commit(instance, now, "actor_sequence_enqueued");
        } else if (abandon_reserved_delivery(instance, reservation)) {
            refuse_delivery(instance,
                            "actor_sequence_refused",
                            devices::status_name(status),
                            host::EffectOutcome::refused);
        }
        return;
    }
    case lua_vm::IntentKind::playPerformance: {
        host::ScriptableOutputReservation reservation{};
        if (!reserve_delivery(instance, reservation)) {
            if (instance.programStatus == ProgramStatus::loaded) {
                refuse_delivery(instance,
                                "performance_refused",
                                "host_reservation_unavailable",
                                host::EffectOutcome::refused);
            }
            return;
        }
        const scenes::SceneStatus status = scenes::play_performance_slot_reserved(
            instance.view, intent.firstRow, intent.secondRow, reservation);
        if (status == scenes::SceneStatus::queued) {
            await_host_commit(instance, now, "performance_enqueued");
        } else if (!abandon_reserved_delivery(instance, reservation)) {
            return;
        } else if (scene_lease_still_publishing(status)) {
            report_intent_status(
                instance, kIntentStatusSceneLeasePending, scenes::status_name(status));
        } else {
            refuse_delivery(instance,
                            "performance_refused",
                            scenes::status_name(status),
                            host::EffectOutcome::refused);
        }
        return;
    }
    case lua_vm::IntentKind::resetObjectives: {
        host::ScriptableOutputReservation reservation{};
        if (!reserve_delivery(instance, reservation)) {
            if (instance.programStatus == ProgramStatus::loaded) {
                refuse_delivery(instance,
                                "objective_reset_refused",
                                "host_reservation_unavailable",
                                host::EffectOutcome::refused);
            }
            return;
        }
        const scenes::SceneStatus status =
            scenes::reset_objectives_slot_reserved(instance.view, intent.firstRow, reservation);
        if (status == scenes::SceneStatus::queued) {
            await_host_commit(instance, now, "objective_reset_enqueued");
        } else if (!abandon_reserved_delivery(instance, reservation)) {
            return;
        } else if (scene_lease_still_publishing(status)) {
            report_intent_status(
                instance, kIntentStatusSceneLeasePending, scenes::status_name(status));
        } else {
            refuse_delivery(instance,
                            "objective_reset_refused",
                            scenes::status_name(status),
                            host::EffectOutcome::refused);
        }
        return;
    }
    case lua_vm::IntentKind::advanceTask:
    case lua_vm::IntentKind::playDialogueCue: {
        const bool dialogue = intent.kind == lua_vm::IntentKind::playDialogueCue;
        const std::uint16_t cueIndex = static_cast<std::uint16_t>(intent.secondRow);
        host::ScriptableOutputReservation reservation{};
        if (!reserve_delivery(instance, reservation)) {
            if (instance.programStatus == ProgramStatus::loaded) {
                refuse_delivery(instance,
                                dialogue ? "dialogue_refused" : "task_refused",
                                "host_reservation_unavailable",
                                host::EffectOutcome::refused);
            }
            return;
        }
        const scenes::SceneStatus status =
            dialogue
                ? scenes::play_dialogue_cue_slot_reserved(
                      instance.view, intent.firstRow, cueIndex, reservation)
                : scenes::activate_task_slot_reserved(instance.view, intent.firstRow, reservation);
        if (status == scenes::SceneStatus::queued) {
            await_host_commit(instance, now, dialogue ? "dialogue_enqueued" : "task_enqueued");
        } else if (!abandon_reserved_delivery(instance, reservation)) {
            return;
        } else if (scene_lease_still_publishing(status)) {
            report_intent_status(
                instance, kIntentStatusSceneLeasePending, scenes::status_name(status));
        } else {
            refuse_delivery(instance,
                            dialogue ? "dialogue_refused" : "task_refused",
                            scenes::status_name(status),
                            host::EffectOutcome::refused);
        }
        return;
    }
    case lua_vm::IntentKind::applySlotAuth: {
        if (intent.authByteCount == 0 || intent.authByteCount != intent.authBody.size()) {
            refuse_delivery(
                instance, "slot_auth_refused", "invalid_body", host::EffectOutcome::refused);
            return;
        }
        const std::span<const std::byte> body(intent.authBody.data(), intent.authByteCount);
        const std::span<const std::byte> sdkBuild(intent.sdkBuildSha256);
        host::ScriptableOutputReservation reservation{};
        if (!reserve_delivery(instance, reservation)) {
            if (instance.programStatus == ProgramStatus::loaded) {
                refuse_delivery(instance,
                                "slot_auth_refused",
                                "host_reservation_unavailable",
                                host::EffectOutcome::refused);
            }
            return;
        }
        const devices::Status status = devices::apply_auth_reserved(instance.view,
                                                                    intent.firstRow,
                                                                    intent.objectTag,
                                                                    intent.registryKey,
                                                                    intent.authSchema,
                                                                    intent.slotIndex,
                                                                    intent.slotType,
                                                                    body,
                                                                    intent.authBitCount,
                                                                    sdkBuild,
                                                                    reservation);
        if (status == devices::Status::queued) {
            await_host_commit(instance, now, "slot_auth_enqueued");
        } else if (!abandon_reserved_delivery(instance, reservation)) {
            return;
        } else {
            refuse_delivery(instance,
                            "slot_auth_refused",
                            devices::status_name(status),
                            host::EffectOutcome::refused);
        }
        return;
    }
    case lua_vm::IntentKind::setLifetime: {
        host::ScriptableOutputReservation reservation{};
        if (!reserve_delivery(instance, reservation)) {
            if (instance.programStatus == ProgramStatus::loaded) {
                refuse_delivery(instance,
                                "lifetime_refused",
                                "host_reservation_unavailable",
                                host::EffectOutcome::refused);
            }
            return;
        }
        const devices::Status status =
            lifetimes::set_reserved(instance.view, intent.lifetimeState, reservation);
        if (status == devices::Status::queued) {
            await_host_commit(instance, now, "lifetime_enqueued");
        } else if (!abandon_reserved_delivery(instance, reservation)) {
            return;
        } else {
            refuse_delivery(instance,
                            "lifetime_refused",
                            devices::status_name(status),
                            host::EffectOutcome::refused);
        }
        return;
    }
    case lua_vm::IntentKind::stopAuthoredScene:
    case lua_vm::IntentKind::signalAuthoredScene:
    case lua_vm::IntentKind::activateAuthoredScene: {
        host::ScriptableOutputReservation reservation{};
        if (!reserve_delivery(instance, reservation)) {
            if (instance.programStatus == ProgramStatus::loaded) {
                refuse_delivery(instance,
                                "scene_refused",
                                "host_reservation_unavailable",
                                host::EffectOutcome::refused);
            }
            return;
        }
        const scenes::SceneStatus status = scenes::activate_authored_scene_reserved(
            instance.view,
            intent.firstRow,
            intent.secondRow,
            reservation,
            intent.sceneEventKey,
            intent.kind == lua_vm::IntentKind::stopAuthoredScene);
        if (status == scenes::SceneStatus::queued) {
            await_host_commit(instance, now, "scene_enqueued");
        } else if (!abandon_reserved_delivery(instance, reservation)) {
            return;
        } else if (scene_lease_still_publishing(status)) {
            report_intent_status(
                instance, kIntentStatusSceneLeasePending, scenes::status_name(status));
        } else {
            refuse_delivery(instance,
                            "scene_refused",
                            scenes::status_name(status),
                            host::EffectOutcome::refused);
        }
        return;
    }
    }
}

} // namespace sunrise::server::activity::mission
