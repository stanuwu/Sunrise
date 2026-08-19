#include <cmath>
#include <limits>
#include <variant>

#include "command_validation_state.h"

namespace sunrise::server::gameplay::physics::host::detail {
namespace {

[[nodiscard]] bool has_actor(std::span<const world::ActorSnapshot> actors,
                             world::ActorKey actor) noexcept {
    for (const world::ActorSnapshot& candidate : actors) {
        if (world::equal_actor(candidate.key, actor)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool finite_vector(world::Vector3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] world::HostCommandExecutionResult reject(world::CommandRejectReason reason) noexcept {
    world::HostCommandExecutionResult result{};
    result.rejectReason = reason;
    result.status = world::HostCommandExecutionStatus::rejected;
    return result;
}

[[nodiscard]] bool incident_allowed(const BubbleHost::Storage::WorldSlot& slot,
                                    std::uint64_t incidentId) noexcept {
    for (std::size_t index = 0; index < slot.incidentCount; ++index) {
        if (slot.incidentAllowlist[index] == incidentId) {
            return true;
        }
    }
    return false;
}

/** @return True for one exact policy-owned combat profile. */
[[nodiscard]] bool combat_profile_available(const BubbleHost::Storage::WorldSlot& slot,
                                            world::BlueprintRef profile,
                                            world::PolicyOwnerId owner) noexcept {
    for (const BubbleHost::Storage::CombatProfileSlot& candidate : slot.combatProfiles) {
        if (candidate.occupied && candidate.profile.profile.id == profile.id
            && candidate.profile.profile.version == profile.version
            && candidate.profile.policyOwnerId == owner) {
            return true;
        }
    }
    return false;
}

} // namespace

/** Validates one extension command without mutating its generic service. */
world::HostCommandExecutionResult
validate_extension_command(const BubbleHost::Storage::WorldSlot& slot,
                           const world::HostExecutionContext& context,
                           const world::HostCommand& command,
                           std::span<const world::ActorSnapshot> actors) noexcept {
    if (context.triggerBudget != slot.triggerBudget || context.queryBudget != slot.queryBudget
        || context.counterBudget != slot.counterBudget
        || context.creditBudget != slot.creditBudget) {
        return reject(world::CommandRejectReason::invalid);
    }

    bool valid = false;
    switch (world::command_kind(command)) {
    case world::HostCommandKind::configureController: {
        const auto& value = std::get<world::ConfigureControllerCommand>(command.payload);
        valid = (slot.controller != nullptr || valid_builtin_controller_configuration(slot, value))
                && has_actor(actors, value.actor) && world::valid_blueprint(value.controller)
                && finite_vector(value.targetPosition)
                && (!world::valid_actor(value.targetActor) || has_actor(actors, value.targetActor));
        break;
    }
    case world::HostCommandKind::requestPath: {
        if (slot.queryCommandCount >= context.queryBudget) {
            return reject(world::CommandRejectReason::capacity);
        }
        const auto& value = std::get<world::RequestPathCommand>(command.payload);
        valid = has_actor(actors, value.actor) && world::valid_blueprint(value.navigationProfile)
                && finite_vector(value.destination);
        break;
    }
    case world::HostCommandKind::configureCombat: {
        const auto& value = std::get<world::ConfigureCombatCommand>(command.payload);
        valid = has_actor(actors, value.actor) && world::valid_blueprint(value.combatProfile)
                && combat_profile_available(slot, value.combatProfile, command.header.policyOwnerId)
                && valid_combat_configuration(slot, command);
        break;
    }
    case world::HostCommandKind::submitDamage: {
        if (slot.combatCommandCount >= mechanics::kCommandDeduplicationCapacity) {
            return reject(world::CommandRejectReason::capacity);
        }
        const auto& value = std::get<world::SubmitDamageCommand>(command.payload);
        valid = has_actor(actors, value.sourceActor) && has_actor(actors, value.targetActor)
                && value.claimedTick != 0 && value.claimedTick <= context.tick
                && registered_attack_matches(slot, value, command.header.policyOwnerId)
                && slot.millimetersPerUnit > 0.0F;
        break;
    }
    case world::HostCommandKind::createTrigger: {
        const mechanics::TriggerSystemSnapshot snapshot = slot.triggers.snapshot();
        std::size_t triggerCount = 0;
        for (const mechanics::TriggerSlotSnapshot& trigger : snapshot.slots) {
            triggerCount += trigger.active ? 1U : 0U;
        }
        if (triggerCount >= context.triggerBudget
            || slot.triggerCommandCount >= mechanics::kCommandDeduplicationCapacity) {
            return reject(world::CommandRejectReason::capacity);
        }
        const auto& value = std::get<world::CreateTriggerCommand>(command.payload);
        valid = value.triggerId != 0 && world::valid_blueprint(value.triggerProfile)
                && world::finite_transform(value.transform) && finite_vector(value.halfExtents)
                && value.halfExtents.x > 0.0F && value.halfExtents.y > 0.0F
                && value.halfExtents.z > 0.0F && valid_trigger_effect(slot, command);
        break;
    }
    case world::HostCommandKind::removeTrigger: {
        if (slot.triggerCommandCount >= mechanics::kCommandDeduplicationCapacity) {
            return reject(world::CommandRejectReason::capacity);
        }
        const auto& value = std::get<world::RemoveTriggerCommand>(command.payload);
        valid = value.triggerId != 0 && value.triggerGeneration != 0
                && valid_trigger_effect(slot, command);
        break;
    }
    case world::HostCommandKind::addObjectiveCounter:
    case world::HostCommandKind::setObjectiveCounter: {
        if (slot.objectiveCommandCount >= mechanics::kCommandDeduplicationCapacity) {
            return reject(world::CommandRejectReason::capacity);
        }
        const mechanics::ObjectiveLedgerSnapshot snapshot = slot.objectives.snapshot();
        std::size_t count = 0;
        for (const mechanics::ObjectiveCounterSnapshot& counter : snapshot.counters) {
            count += counter.active ? 1U : 0U;
        }
        if (count == 0 || count > context.counterBudget) {
            return reject(world::CommandRejectReason::capacity);
        }
        if (world::command_kind(command) == world::HostCommandKind::addObjectiveCounter) {
            const auto& value = std::get<world::AddObjectiveCounterCommand>(command.payload);
            valid = value.counterId != 0 && value.amount != 0 && value.expectedRevision != 0
                    && valid_objective_effect(slot, command);
        } else {
            const auto& value = std::get<world::SetObjectiveCounterCommand>(command.payload);
            valid = value.counterId != 0 && value.expectedRevision != 0
                    && valid_objective_effect(slot, command);
        }
        break;
    }
    case world::HostCommandKind::recordParticipantCredit: {
        if (slot.creditCommandCount >= mechanics::kCommandDeduplicationCapacity) {
            return reject(world::CommandRejectReason::capacity);
        }
        const mechanics::ParticipantCreditLedgerSnapshot snapshot = slot.credits.snapshot();
        std::size_t count = 0;
        for (const mechanics::ParticipantCreditSnapshot& participant : snapshot.participants) {
            count += participant.active ? 1U : 0U;
        }
        if (count == 0 || count > context.creditBudget) {
            return reject(world::CommandRejectReason::capacity);
        }
        const auto& value = std::get<world::RecordParticipantCreditCommand>(command.payload);
        valid = value.participantId != 0 && value.channelId != 0 && value.amount != 0
                && value.channelId <= std::numeric_limits<std::uint32_t>::max()
                && valid_credit_effect(slot, command);
        break;
    }
    case world::HostCommandKind::emitMappedIncident: {
        const auto& value = std::get<world::EmitMappedIncidentCommand>(command.payload);
        valid = incident_allowed(slot, value.incidentId) && has_actor(actors, value.sourceActor)
                && (!world::valid_actor(value.targetActor) || has_actor(actors, value.targetActor));
        break;
    }
    case world::HostCommandKind::scheduleTickTimer: {
        const mechanics::TickTimerServiceSnapshot snapshot = slot.timers.snapshot();
        std::size_t count = 0;
        for (const mechanics::TickTimerSlotSnapshot& timer : snapshot.slots) {
            count += timer.active ? 1U : 0U;
        }
        if (count >= mechanics::kTickTimerCapacity
            || slot.timerCommandCount >= mechanics::kCommandDeduplicationCapacity) {
            return reject(world::CommandRejectReason::capacity);
        }
        const auto& value = std::get<world::ScheduleTickTimerCommand>(command.payload);
        valid = value.timerId != 0 && value.timerGeneration != 0 && value.fireTick >= context.tick
                && valid_timer_effect(slot);
        break;
    }
    default:
        break;
    }
    if (!valid) {
        return reject(world::CommandRejectReason::invalid);
    }
    world::HostCommandExecutionResult result{};
    result.rejectReason = world::CommandRejectReason::none;
    result.status = world::HostCommandExecutionStatus::applied;
    return result;
}

/** Checks registered profile, facing target, and exact kinematic body binding. */
bool valid_builtin_controller_configuration(
    const BubbleHost::Storage::WorldSlot& slot,
    const world::ConfigureControllerCommand& command) noexcept {
    const controller::ControllerProfile* profile = nullptr;
    for (const BubbleHost::Storage::ControllerProfileSlot& candidate : slot.controllerProfiles) {
        if (candidate.occupied && candidate.profile.controller.id == command.controller.id
            && candidate.profile.controller.version == command.controller.version) {
            profile = &candidate.profile;
            break;
        }
    }
    if (profile == nullptr
        || (profile->facing == controller::FacingMode::actor
            && !world::valid_actor(command.targetActor))) {
        return false;
    }
    for (const BubbleHost::Storage::BodyBinding& binding : slot.bodies) {
        if (!binding.occupied || !world::equal_actor(binding.actor, command.actor)) {
            continue;
        }
        backend::BodyState body{};
        return slot.physics.read_body(binding.body, body)
               && body.spec.motion == backend::MotionKind::kinematic;
    }
    return false;
}

} // namespace sunrise::server::gameplay::physics::host::detail
