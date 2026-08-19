#include <limits>

#include "combat_kernel.h"

namespace sunrise::server::gameplay::physics::mechanics {

/** Restores one exact dead combatant after all command guards pass. */
CombatResult CombatKernel::respawn(const RespawnIntent& intent,
                                   std::span<CombatEvent> events,
                                   std::size_t& eventCount) noexcept {
    eventCount = 0;
    const auto reject = [&](CombatRejectReason reason, CombatResult result) noexcept {
        if (events.empty()) {
            return CombatResult::outputCapacity;
        }
        CombatEvent event{};
        event.targetActor = intent.targetActor;
        event.worldEpoch = worldEpoch_;
        event.tick = intent.command.executionTick;
        event.commandId = intent.command.commandId;
        event.kind = CombatEventKind::respawnRejected;
        event.rejectReason = reason;
        events[0] = event;
        eventCount = 1;
        return result;
    };
    if (worldEpoch_ == kAbsentWorldEpoch || intent.command.worldEpoch != worldEpoch_) {
        return reject(CombatRejectReason::staleWorld, CombatResult::rejected);
    }
    if (!valid_command_header(intent.command, worldEpoch_)) {
        return reject(CombatRejectReason::invalidCommand, CombatResult::rejected);
    }
    if (commands_.contains(intent.command.policyOwnerId, intent.command.commandId)) {
        return reject(CombatRejectReason::duplicate, CombatResult::duplicate);
    }
    const std::size_t targetIndex = find_actor(intent.targetActor);
    if (targetIndex == combatants_.size()) {
        return reject(CombatRejectReason::invalidTarget, CombatResult::invalidHandle);
    }
    CombatantSnapshot& target = combatants_[targetIndex];
    if (target.definition.policyOwnerId != intent.command.policyOwnerId) {
        return reject(CombatRejectReason::policyOwner, CombatResult::rejected);
    }
    if (target.alive) {
        return reject(CombatRejectReason::targetAlive, CombatResult::noChange);
    }
    if (target.revision == std::numeric_limits<std::uint64_t>::max()) {
        return reject(CombatRejectReason::capacity, CombatResult::capacity);
    }
    if (events.size() < 2) {
        return CombatResult::outputCapacity;
    }
    if (commands_.remember(intent.command) != DeduplicationResult::recorded) {
        return reject(CombatRejectReason::capacity, CombatResult::capacity);
    }
    const std::uint32_t oldHealth = target.health;
    const std::uint32_t oldShield = target.shield;
    target.health = target.definition.maximumHealth;
    target.shield = target.definition.maximumShield;
    target.alive = true;
    target.nextDamageTick = intent.command.executionTick;
    target.rewindSamples = {};
    target.rewindSampleCount = 0;
    ++target.revision;
    const CombatEvent base{{},
                           intent.targetActor,
                           worldEpoch_,
                           intent.command.executionTick,
                           intent.command.commandId,
                           0,
                           0,
                           target.revision,
                           oldHealth,
                           target.health,
                           oldShield,
                           target.shield,
                           CombatEventKind::respawn,
                           CombatRejectReason::none};
    events[0] = base;
    events[1] = base;
    events[1].kind = CombatEventKind::healthChanged;
    eventCount = 2;
    return CombatResult::applied;
}

void CombatKernel::expire_commands_before(TickId firstRetainedTick) noexcept {
    commands_.expire_before(firstRetainedTick);
}

} // namespace sunrise::server::gameplay::physics::mechanics
