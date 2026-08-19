#include "bubble_host.h"
#include "internal.h"

namespace sunrise::server::gameplay::physics::host {

/** Copies pointer-free generic-service state for a logical checkpoint. */
HostStatus BubbleHost::snapshot_services(WorldHandle handle,
                                         ServiceSnapshot& output) const noexcept {
    output = {};
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    const Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    if (!slot.builtInController.snapshot(output.controllers)) {
        return HostStatus::unhealthy;
    }
    output.timers = slot.timers.snapshot();
    output.triggers = slot.triggers.snapshot();
    output.objectives = slot.objectives.snapshot();
    output.credits = slot.credits.snapshot();
    output.combat = slot.combat.snapshot();
    return HostStatus::success;
}

/** Restores compatible generic services before the fresh world runs. */
HostStatus BubbleHost::restore_services(WorldHandle handle,
                                        const ServiceSnapshot& source) noexcept {
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    if (slot.runner.tick() != 0 || slot.inTick) {
        return HostStatus::invalidArgument;
    }

    // Host-owned scratch, not a per-call allocation: this runs on a game thread.
    ServiceSnapshot& rebound = storage_->restoreScratch;
    rebound = source;
    const mechanics::WorldEpoch epoch = slot.runner.identity().worldGeneration;
    rebound.timers.worldEpoch = epoch;
    rebound.triggers.worldEpoch = epoch;
    rebound.objectives.worldEpoch = epoch;
    rebound.credits.worldEpoch = epoch;
    rebound.combat.worldEpoch = epoch;
    for (const mechanics::TriggerSlotSnapshot& trigger : rebound.triggers.slots) {
        if (trigger.active) {
            return HostStatus::serviceRejected;
        }
    }
    if (rebound.controllers.controllerCount != 0) {
        return HostStatus::serviceRejected;
    }
    mechanics::TickTimerService timers{};
    mechanics::TriggerSystem triggers{};
    mechanics::ObjectiveLedger objectives{};
    mechanics::ParticipantCreditLedger credits{};
    mechanics::CombatKernel combat{};
    if (!timers.restore(rebound.timers) || !triggers.restore(rebound.triggers)
        || !objectives.restore(rebound.objectives) || !credits.restore(rebound.credits)
        || !combat.restore(rebound.combat)) {
        return HostStatus::serviceRejected;
    }
    if (!slot.builtInController.restore(rebound.controllers).succeeded()) {
        return HostStatus::serviceRejected;
    }
    slot.timers = timers;
    slot.triggers = triggers;
    slot.objectives = objectives;
    slot.credits = credits;
    slot.combat = combat;
    slot.triggerBindings = {};
    slot.attackProfiles = {};
    slot.combatants = {};

    for (std::size_t index = 0; index < rebound.combat.attacks.size(); ++index) {
        const mechanics::AttackBlueprintSnapshot& attack = rebound.combat.attacks[index];
        if (!attack.active) {
            continue;
        }
        Storage::AttackProfileSlot& binding = slot.attackProfiles[index];
        binding.profile.attack = {attack.definition.attackBlueprintId,
                                  attack.definition.blueprintVersion};
        binding.profile.policyOwnerId = attack.definition.policyOwnerId;
        binding.profile.damage = attack.definition.damage;
        binding.profile.maximumRangeMillimeters = attack.definition.maximumRangeMillimeters;
        binding.profile.cooldownTicks = attack.definition.cooldownTicks;
        binding.attack = {attack.generation, static_cast<std::uint16_t>(index)};
        binding.occupied = true;
    }
    for (std::size_t index = 0; index < rebound.combat.combatants.size(); ++index) {
        const mechanics::CombatantSnapshot& combatant = rebound.combat.combatants[index];
        if (!combatant.active) {
            continue;
        }
        Storage::CombatantBinding& binding = slot.combatants[index];
        binding.actor = {combatant.definition.actor.actorId, combatant.definition.actor.generation};
        binding.combatant = {combatant.generation, static_cast<std::uint16_t>(index)};
        binding.occupied = true;
    }
    return HostStatus::success;
}

/** Advances world and mechanics replay retention under one exact handle. */
HostStatus BubbleHost::expire_commands_before(WorldHandle handle,
                                              world::TickId firstRetainedTick) noexcept {
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    if (slot.inTick || firstRetainedTick == 0) {
        return HostStatus::invalidArgument;
    }
    if (!slot.runner.expire_commands_before(firstRetainedTick)) {
        return HostStatus::invalidArgument;
    }
    slot.timers.expire_commands_before(firstRetainedTick);
    slot.triggers.expire_commands_before(firstRetainedTick);
    slot.objectives.expire_commands_before(firstRetainedTick);
    slot.credits.expire_commands_before(firstRetainedTick);
    slot.combat.expire_commands_before(firstRetainedTick);
    return HostStatus::success;
}

} // namespace sunrise::server::gameplay::physics::host
