#include <cstdint>
#include <limits>
#include <string_view>

#include "../../bap/runtime.h"
#include "../../gameplay/squad_entity_retirement.h"
#include "mission_script_runtime_internal.h"

// The delivery state machine: the four stages and the timeout reconcilers. A
// script owns one intent at a time, so every step reads and writes one instance. The intent
// fan-out, the instance table and the service loop stay elsewhere.

namespace sunrise::server::activity::mission {

/** @return now plus delay, saturated at the maximum instead of wrapping. */
[[nodiscard]] std::uint64_t deadline_after(std::uint64_t now, std::uint64_t delay) noexcept {
    return now > (std::numeric_limits<std::uint64_t>::max)() - delay
               ? (std::numeric_limits<std::uint64_t>::max)()
               : now + delay;
}

namespace {

/** How long a cancelled Host output may take to confirm before the delivery faults. */
constexpr std::uint64_t kCancelTimeoutMs = 2'000;
/** Recheck delay while a cancel request could not be queued yet. */
constexpr std::uint64_t kCancelRetryMs = 250;
/** How long one intent may stay in delivery before it is refused. */
constexpr std::uint64_t kIntentLifetimeMs = 60'000;
/** Retired head events compact only past this count, so small queues never reallocate. */
constexpr std::size_t kScriptEventCompactionThreshold = 64;

/** Acknowledges only the exact durable head and Host output revision. */
[[nodiscard]] bool acknowledge_delivery_state(RuntimeInstance& instance) noexcept {
    mission_state::Snapshot snapshot{};
    const mission_state::Status status =
        mission_state::acknowledge_intent_output(instance.view.binding,
                                                 instance.programKey,
                                                 instance.missionStateRevision,
                                                 instance.durableIntentSequence,
                                                 instance.expectedScriptableRevision,
                                                 snapshot);
    if (status != mission_state::Status::ready) {
        // This attachment lost its compare. Leave authoritative State unchanged so an exact
        // reattach can reconcile the retained Host transport revision without rerunning Lua.
        lua_vm::fault(instance.vm, "durable mission intent acknowledgement compare was refused");
        instance.programStatus = ProgramStatus::programError;
        log_line(
            core::log::Level::warn, &instance, "intent_ack", mission_state::status_name(status));
        return false;
    }
    accept_mission_state(instance, snapshot);
    return true;
}

/**
 * Stages one effectResult event for the script that raised the intent.
 * The event replaces nothing already staged, because a program runs one delivery at a time. The
 * service tick is stamped when the event is dispatched, not here.
 */
void queue_effect_result(RuntimeInstance& instance,
                         const lua_vm::Intent& intent,
                         host::EffectOutcome outcome) noexcept {
    if (intent.requestKey == mission_state::kAbsentIntentKey) {
        return;
    }
    host::Event event{};
    event.binding = instance.view.binding;
    event.sequence = intent.requestKey;
    event.sourceGeneration = instance.view.activityClientGeneration;
    event.missionSequence = instance.lastMissionSequence;
    event.effectRequestKey = intent.requestKey;
    event.effectAction = static_cast<std::uint8_t>(intent.kind);
    event.effectOutcome = outcome;
    event.kind = host::EventKind::effectResult;
    push_script_event(instance, event);
}

} // namespace

/** Releases an exact unstaged Host revision while retaining the durable intent. */
[[nodiscard]] bool release_delivery_state(RuntimeInstance& instance) noexcept {
    if (instance.expectedScriptableRevision == 0) {
        return true;
    }
    mission_state::Snapshot snapshot{};
    const mission_state::Status status =
        mission_state::release_intent_output(instance.view.binding,
                                             instance.programKey,
                                             instance.missionStateRevision,
                                             instance.durableIntentSequence,
                                             instance.expectedScriptableRevision,
                                             snapshot);
    if (status != mission_state::Status::ready) {
        lua_vm::fault(instance.vm, "durable mission intent release compare was refused");
        instance.programStatus = ProgramStatus::programError;
        log_line(core::log::Level::warn,
                 &instance,
                 "intent_release",
                 mission_state::status_name(status));
        return false;
    }
    accept_mission_state(instance, snapshot);
    // The durable head owns no Host revision now, so a later step must not release it twice.
    instance.expectedScriptableRevision = 0;
    return true;
}

/** Returns the instance to the idle stage and clears delivery timing. */
void clear_delivery(RuntimeInstance& instance) noexcept {
    instance.expectedScriptableRevision = 0;
    instance.deliveryDeadline = 0;
    instance.firstIntentAttempt = 0;
    instance.nextIntentAttempt = 0;
    instance.intentAttempts = 0;
    instance.lastIntentStatus = (std::numeric_limits<std::uint16_t>::max)();
    instance.deliveryStage = DeliveryStage::idle;
}

/** Retires one idempotently applied local effect and emits its terminal result. */
bool complete_local_effect(RuntimeInstance& instance, std::string_view result) noexcept {
    lua_vm::Intent intent{};
    if (!lua_vm::pending_intent(instance.vm, intent)
        || instance.durableIntentSequence == mission_state::kAbsentIntentSequence
        || instance.durableHostOutputRevision != mission_state::kAbsentHostOutputRevision
        || instance.expectedScriptableRevision != 0) {
        fault_delivery(instance,
                       "local_effect_mismatch",
                       "local effect did not match the unassigned durable intent head");
        return false;
    }
    mission_state::Snapshot snapshot{};
    const mission_state::Status status =
        mission_state::acknowledge_intent(instance.view.binding,
                                          instance.programKey,
                                          instance.missionStateRevision,
                                          instance.durableIntentSequence,
                                          snapshot);
    if (status != mission_state::Status::ready) {
        fault_delivery(instance, "local_effect_ack", mission_state::status_name(status));
        return false;
    }
    accept_mission_state(instance, snapshot);
    lua_vm::consume_intent(instance.vm);
    ++instance.intentsTransportStaged;
    log_line(core::log::Level::info, &instance, "delivery", result);
    queue_effect_result(instance, intent, host::EffectOutcome::transportStaged);
    clear_delivery(instance);
    return true;
}

/** Faults the program and abandons the delivery. */
void fault_delivery(RuntimeInstance& instance,
                    std::string_view result,
                    std::string_view reason) noexcept {
    fault_instance(instance, reason);
    instance.programStatus = ProgramStatus::programError;
    clear_pending_events(instance.view.binding);
    clear_delivery(instance);
    log_line(core::log::Level::warn, &instance, "delivery", result, {}, reason);
}

/**
 * Logs one adapter status, and only when it differs from the last one logged.
 * @param status Adapter status, biased so each adapter owns its own range.
 * @param name Diagnostic name of that status.
 */
void report_intent_status(RuntimeInstance& instance,
                          std::uint16_t status,
                          std::string_view name) noexcept {
    if (instance.lastIntentStatus == status) {
        return;
    }
    instance.lastIntentStatus = status;
    log_line(core::log::Level::debug, &instance, "intent", name);
}

/**
 * @param now Current service tick.
 * @return Whether the intent has been in delivery longer than its lifetime allows.
 */
[[nodiscard]] bool intent_lifetime_expired(const RuntimeInstance& instance,
                                           std::uint64_t now) noexcept {
    return instance.intentAttempts != 0
           && now >= deadline_after(instance.firstIntentAttempt, kIntentLifetimeMs);
}

namespace {

/** Clears one VM and State head only after the same Host revision reached transport. */
void complete_delivery(RuntimeInstance& instance) noexcept {
    lua_vm::Intent intent{};
    if (!lua_vm::pending_intent(instance.vm, intent)) {
        fault_delivery(instance, "missing_intent", "transport staged without a pending intent");
        return;
    }
    if (instance.expectedScriptableRevision == 0
        || instance.durableIntentSequence == mission_state::kAbsentIntentSequence
        || instance.durableHostOutputRevision != instance.expectedScriptableRevision) {
        lua_vm::fault(instance.vm, "transport stage did not match durable mission State");
        instance.programStatus = ProgramStatus::programError;
        log_line(core::log::Level::warn, &instance, "intent_ack", "durable_mismatch");
        clear_pending_events(instance.view.binding);
        clear_delivery(instance);
        return;
    }
    if (!acknowledge_delivery_state(instance)) {
        clear_pending_events(instance.view.binding);
        clear_delivery(instance);
        return;
    }
    lua_vm::consume_intent(instance.vm);
    const char* result = nullptr;
    switch (intent.kind) {
    case lua_vm::IntentKind::placeSquad:
        result = "squad_staged";
        break;
    case lua_vm::IntentKind::actorCommand:
        result = "actor_command_staged";
        break;
    case lua_vm::IntentKind::bindCombatantToSquad:
        result = "combatant_binding_staged";
        break;
    case lua_vm::IntentKind::stopAuthoredScene:
        result = "scene_stop_staged";
        break;
    case lua_vm::IntentKind::signalAuthoredScene:
        result = "scene_event_staged";
        break;
    case lua_vm::IntentKind::activateAuthoredScene:
        result = "scene_staged";
        break;
    case lua_vm::IntentKind::setObjectActive:
        result = "object_staged";
        break;
    case lua_vm::IntentKind::setDeviceChannel:
        result = "device_staged";
        break;
    case lua_vm::IntentKind::applySlotAuth:
        result = "slot_auth_staged";
        break;
    case lua_vm::IntentKind::setLifetime:
        result = "lifetime_staged";
        break;
    case lua_vm::IntentKind::restartCheckpoint:
        result = "checkpoint_staged";
        break;
    case lua_vm::IntentKind::fireTrigger:
        result = "trigger_staged";
        break;
    case lua_vm::IntentKind::playSequence:
        result = "sequence_staged";
        break;
    case lua_vm::IntentKind::setCinematicActive:
        result = "cinematic_staged";
        break;
    case lua_vm::IntentKind::playPerformance:
        result = "performance_staged";
        break;
    case lua_vm::IntentKind::playActorSequence:
        result = "actor_sequence_staged";
        break;
    case lua_vm::IntentKind::resetObjectives:
        result = "objective_reset_staged";
        break;
    case lua_vm::IntentKind::advanceTask:
        result = "task_staged";
        break;
    case lua_vm::IntentKind::playDialogueCue:
        result = "dialogue_staged";
        break;
    case lua_vm::IntentKind::selectMissionState:
        result = "state_selected";
        break;
    }
    ++instance.intentsTransportStaged;
    log_line(core::log::Level::info, &instance, "delivery", result);
    queue_effect_result(instance, intent, host::EffectOutcome::transportStaged);
    clear_delivery(instance);
}

/** Atomically excludes a refused queued Host output, then retires the intent exactly once. */
void terminate_refused_delivery(RuntimeInstance& instance,
                                std::uint64_t now,
                                std::string_view result,
                                std::string_view reason,
                                host::EffectOutcome outcome) noexcept {
    const host::ScriptableWithdrawStatus withdrawn = host::withdraw_scriptable_output(
        instance.view.binding, instance.durableIntentSequence, instance.expectedScriptableRevision);
    if (withdrawn == host::ScriptableWithdrawStatus::transportStaged) {
        complete_delivery(instance);
        return;
    }
    if (withdrawn == host::ScriptableWithdrawStatus::committed) {
        instance.deliveryStage = DeliveryStage::awaitingTransport;
        instance.deliveryDeadline = deadline_after(now, kTransportTimeoutMs);
        log_line(core::log::Level::debug, &instance, "delivery", "commit_race_reconciled");
        return;
    }
    if (withdrawn == host::ScriptableWithdrawStatus::advanced
        || withdrawn == host::ScriptableWithdrawStatus::mismatch) {
        fault_delivery(instance,
                       "withdraw_mismatch",
                       "Host could not withdraw the exact queued mission output");
        return;
    }
    refuse_delivery(instance, result, reason, outcome);
}

} // namespace

/**
 * Reports one refused request to the script and keeps the program running.
 * The durable head is dropped, so the outbox advances. A State compare that refuses the drop is a
 * real inconsistency and still faults.
 */
void refuse_delivery(RuntimeInstance& instance,
                     std::string_view result,
                     std::string_view reason,
                     host::EffectOutcome outcome) noexcept {
    lua_vm::Intent intent{};
    if (!lua_vm::pending_intent(instance.vm, intent)) {
        fault_delivery(instance, result, reason);
        return;
    }
    if (instance.expectedScriptableRevision != 0 && !release_delivery_state(instance)) {
        return;
    }
    if (instance.durableIntentSequence != mission_state::kAbsentIntentSequence) {
        mission_state::Snapshot snapshot{};
        const mission_state::Status status =
            mission_state::discard_intent(instance.view.binding,
                                          instance.programKey,
                                          instance.missionStateRevision,
                                          instance.durableIntentSequence,
                                          snapshot);
        if (status != mission_state::Status::ready) {
            fault_delivery(instance, "discard_refused", mission_state::status_name(status));
            return;
        }
        accept_mission_state(instance, snapshot);
    }
    lua_vm::consume_intent(instance.vm);
    queue_effect_result(instance, intent, outcome);
    if (intent.retirePlacedProps) {
        server::gameplay::squad_entity_retirement::cancel_placed_transition(
            instance.view.binding, instance.view.activityClientGeneration, intent.requestKey);
    }
    clear_delivery(instance);
    log_line(core::log::Level::warn, &instance, "intent_refused", result, {}, reason);
}

/**
 * Reconciles an exact transport stage whose Host event cursor was reset on reattach.
 * @return Whether the delivery was completed here.
 */
[[nodiscard]] bool reconcile_transport_stage(RuntimeInstance& instance) noexcept {
    if (instance.deliveryStage == DeliveryStage::idle || instance.expectedScriptableRevision == 0) {
        return false;
    }
    host::InstanceSnapshot hostView{};
    if (host::instance_snapshot(instance.view.binding, hostView)
        && hostView.scriptableTransportRevision == instance.expectedScriptableRevision) {
        complete_delivery(instance);
        return true;
    }
    return false;
}

/**
 * Reconciles a lost stage event and cancels an exact unstaged Host body at hard expiry.
 * @return Whether the caller must stop, because the delivery was completed or left owned.
 */
[[nodiscard]] bool reconcile_expired_delivery(RuntimeInstance& instance) noexcept {
    if (instance.deliveryStage == DeliveryStage::idle || instance.expectedScriptableRevision == 0) {
        return false;
    }
    const host::ScriptableWithdrawStatus withdrawn = host::withdraw_scriptable_output(
        instance.view.binding, instance.durableIntentSequence, instance.expectedScriptableRevision);
    if (withdrawn == host::ScriptableWithdrawStatus::transportStaged) {
        complete_delivery(instance);
        return true;
    }
    if (withdrawn == host::ScriptableWithdrawStatus::withdrawn
        || withdrawn == host::ScriptableWithdrawStatus::absent
        || withdrawn == host::ScriptableWithdrawStatus::canceled) {
        // Preserve the typed diagnostic head but remove ownership of an output that cannot run.
        return !release_delivery_state(instance);
    }
    if (withdrawn != host::ScriptableWithdrawStatus::committed) {
        return false;
    }
    if (server::bap::cancel_activity_scriptable_override(instance.view.binding,
                                                         instance.expectedScriptableRevision)) {
        return !release_delivery_state(instance);
    }
    host::InstanceSnapshot reconciled{};
    if (host::instance_snapshot(instance.view.binding, reconciled)
        && reconciled.scriptableTransportRevision == instance.expectedScriptableRevision) {
        complete_delivery(instance);
        return true;
    }
    return false;
}

/**
 * Advances the delivery stage whose deadline has passed.
 * @param now Current service tick.
 * @return Whether a deadline fired, so no further work is owed this tick.
 */
[[nodiscard]] bool service_delivery_timeout(RuntimeInstance& instance, std::uint64_t now) noexcept {
    if (instance.deliveryStage == DeliveryStage::idle || now < instance.deliveryDeadline) {
        return false;
    }
    host::InstanceSnapshot hostView{};
    if (!host::instance_snapshot(instance.view.binding, hostView)) {
        fault_delivery(instance, "host_missing", "Host instance vanished during delivery");
        return true;
    }
    if (hostView.scriptableTransportRevision == instance.expectedScriptableRevision) {
        complete_delivery(instance);
        return true;
    }
    if (hostView.scriptableRevision > instance.expectedScriptableRevision) {
        fault_delivery(instance, "revision_advanced", "Host advanced past the queued intent");
        return true;
    }
    if (instance.deliveryStage == DeliveryStage::awaitingHostCommit) {
        if (hostView.scriptableRevision == instance.expectedScriptableRevision
            && hostView.outputPending
            && hostView.outputKind == host::OutputKind::scriptableOverride) {
            instance.deliveryStage = DeliveryStage::awaitingTransport;
            instance.deliveryDeadline = deadline_after(now, kTransportTimeoutMs);
            log_line(core::log::Level::debug, &instance, "delivery", "commit_reconciled");
        } else if (hostView.scriptableRevision == instance.expectedScriptableRevision
                   && !hostView.outputPending) {
            // The control applied and the output was then dropped. Nothing is left to stage.
            refuse_delivery(instance,
                            "commit_canceled",
                            "Host dropped the committed output before it staged",
                            host::EffectOutcome::canceled);
        } else {
            terminate_refused_delivery(instance,
                                       now,
                                       "commit_missing",
                                       "Host did not commit the queued intent",
                                       host::EffectOutcome::refused);
        }
        return true;
    }
    if (instance.deliveryStage == DeliveryStage::awaitingTransport) {
        if (hostView.scriptableRevision != instance.expectedScriptableRevision
            || (hostView.outputPending
                && hostView.outputKind != host::OutputKind::scriptableOverride)) {
            fault_delivery(
                instance, "host_state_mismatch", "Host state no longer owns the queued intent");
            return true;
        }
        if (!hostView.outputPending) {
            refuse_delivery(instance,
                            "stage_canceled",
                            "Host dropped the committed output before it staged",
                            host::EffectOutcome::canceled);
            return true;
        }
        if (server::bap::cancel_activity_scriptable_override(instance.view.binding,
                                                             instance.expectedScriptableRevision)) {
            instance.deliveryStage = DeliveryStage::awaitingCancel;
            instance.deliveryDeadline = deadline_after(now, kCancelTimeoutMs);
            log_line(core::log::Level::debug, &instance, "delivery", "cancel_requested");
        } else {
            instance.deliveryDeadline = deadline_after(now, kCancelRetryMs);
            report_intent_status(instance, kIntentStatusCancelPending, "cancel_pending");
        }
        return true;
    }
    if (!hostView.outputPending) {
        refuse_delivery(instance,
                        "cancel_reconciled",
                        "Host canceled the timed-out intent",
                        host::EffectOutcome::canceled);
    } else {
        fault_delivery(instance, "cancel_timeout", "Host did not confirm intent cancellation");
    }
    return true;
}

/** Retires the head event after its single delivery attempt. */
void retire_script_event(RuntimeInstance& instance) noexcept {
    if (instance.scriptEventRead < instance.scriptEvents.size()) {
        ++instance.scriptEventRead;
        if (instance.scriptEventRead == instance.scriptEvents.size()) {
            instance.scriptEvents.clear();
            instance.scriptEventRead = 0;
        } else if (instance.scriptEventRead >= kScriptEventCompactionThreshold
                   && instance.scriptEventRead >= instance.scriptEvents.size() / 2) {
            instance.scriptEvents.erase(
                instance.scriptEvents.begin(),
                instance.scriptEvents.begin()
                    + static_cast<std::ptrdiff_t>(instance.scriptEventRead));
            instance.scriptEventRead = 0;
        }
    }
    instance.firstScriptEventAttempt = 0;
    instance.nextScriptEventAttempt = 0;
    instance.scriptEventAttempts = 0;
}

/**
 * Advances the delivery stage on one host event, or faults the delivery.
 * A revision that is zero or not the one the queued intent expects is a fault, never a skip.
 * @param now Service tick the next stage deadline is measured from.
 */
void observe_delivery_event(RuntimeInstance& instance,
                            const host::Event& event,
                            std::uint64_t now) noexcept {
    if (instance.deliveryStage == DeliveryStage::idle) {
        return;
    }
    if (instance.deliveryStage == DeliveryStage::awaitingHostCommit) {
        if (event.kind == host::EventKind::scriptableOverrideCommitted) {
            if (event.scriptableRevision == 0
                || event.scriptableRevision != instance.expectedScriptableRevision) {
                fault_delivery(instance,
                               "invalid_commit",
                               "Host commit did not match the queued intent revision");
                return;
            }
            instance.deliveryDeadline = deadline_after(now, kTransportTimeoutMs);
            instance.deliveryStage = DeliveryStage::awaitingTransport;
            log_line(core::log::Level::debug, &instance, "delivery", "host_committed");
        } else if (event.kind == host::EventKind::scriptableOverrideTransportStaged) {
            if (event.scriptableRevision == 0
                || event.scriptableRevision != instance.expectedScriptableRevision) {
                fault_delivery(instance,
                               "invalid_stage",
                               "transport stage did not match the queued intent revision");
                return;
            }
            complete_delivery(instance);
        } else if (event.kind == host::EventKind::operatorRefused) {
            terminate_refused_delivery(instance,
                                       now,
                                       "host_refused",
                                       "Host refused the queued intent",
                                       host::EffectOutcome::refused);
        }
        return;
    }
    if (event.kind != host::EventKind::scriptableOverrideTransportStaged
        && event.kind != host::EventKind::scriptableOverrideCanceled) {
        return;
    }
    if (event.scriptableRevision == 0
        || event.scriptableRevision != instance.expectedScriptableRevision) {
        fault_delivery(instance, "revision_mismatch", "Host delivery revision did not match");
        return;
    }
    if (event.kind == host::EventKind::scriptableOverrideTransportStaged) {
        complete_delivery(instance);
        return;
    }
    if (instance.deliveryStage == DeliveryStage::awaitingCancel) {
        refuse_delivery(instance,
                        "host_canceled",
                        "Host canceled the timed-out intent",
                        host::EffectOutcome::canceled);
        return;
    }
    refuse_delivery(instance,
                    "host_canceled",
                    "Host discarded the queued output before it staged",
                    host::EffectOutcome::canceled);
}

/**
 * Reconciles an output assigned before a terminal program fault without ever starting a new
 * adapter request. The Host withdraw operation serializes queued-control removal against its
 * reducer; committed output is allowed to finish exactly once and staged output is acknowledged.
 */
void reconcile_terminal_delivery(RuntimeInstance& instance) noexcept {
    if (instance.programStatus != ProgramStatus::programError || !instance.missionStateFaulted
        || instance.durableHostOutputRevision == mission_state::kAbsentHostOutputRevision
        || instance.durableIntentSequence == mission_state::kAbsentIntentSequence) {
        return;
    }
    instance.expectedScriptableRevision = instance.durableHostOutputRevision;
    const host::ScriptableWithdrawStatus status = host::withdraw_scriptable_output(
        instance.view.binding, instance.durableIntentSequence, instance.expectedScriptableRevision);
    if (status == host::ScriptableWithdrawStatus::transportStaged) {
        complete_delivery(instance);
        return;
    }
    if (status == host::ScriptableWithdrawStatus::withdrawn
        || status == host::ScriptableWithdrawStatus::absent
        || status == host::ScriptableWithdrawStatus::canceled) {
        if (release_delivery_state(instance)) {
            clear_delivery(instance);
            log_line(core::log::Level::debug, &instance, "delivery", "terminal_released");
        }
    }
}

} // namespace sunrise::server::activity::mission
