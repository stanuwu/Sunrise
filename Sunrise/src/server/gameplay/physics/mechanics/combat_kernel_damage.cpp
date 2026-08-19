#include <algorithm>
#include <limits>

#include "combat_kernel.h"
#include "combat_kernel_internal.h"

namespace sunrise::server::gameplay::physics::mechanics {

/** Validates and commits one damage command without partial output. */
CombatResult CombatKernel::submit_damage(const DamageIntent& intent,
                                         std::span<CombatEvent> events,
                                         std::size_t& eventCount) noexcept {
    eventCount = 0;
    const auto reject = [&](CombatRejectReason reason, CombatResult result) noexcept {
        if (events.empty()) {
            return CombatResult::outputCapacity;
        }
        CombatEvent event{};
        event.sourceActor = intent.sourceActor;
        event.targetActor = intent.targetActor;
        event.worldEpoch = worldEpoch_;
        event.tick = intent.command.executionTick;
        event.commandId = intent.command.commandId;
        event.kind = CombatEventKind::damageRejected;
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
    const std::size_t sourceIndex = find_actor(intent.sourceActor);
    const std::size_t targetIndex = find_actor(intent.targetActor);
    if (sourceIndex == combatants_.size()) {
        return reject(CombatRejectReason::invalidSource, CombatResult::rejected);
    }
    if (targetIndex == combatants_.size() || sourceIndex == targetIndex) {
        return reject(CombatRejectReason::invalidTarget, CombatResult::rejected);
    }
    if (!valid_attack_handle(intent.attackBlueprint)) {
        return reject(CombatRejectReason::invalidAttack, CombatResult::rejected);
    }
    CombatantSnapshot& source = combatants_[sourceIndex];
    CombatantSnapshot& target = combatants_[targetIndex];
    const AttackBlueprintSnapshot& attack = attacks_[intent.attackBlueprint.slot];
    if (intent.command.policyOwnerId != source.definition.policyOwnerId
        || intent.command.policyOwnerId != attack.definition.policyOwnerId
        || intent.command.blueprintVersion != attack.definition.blueprintVersion) {
        return reject(CombatRejectReason::policyOwner, CombatResult::rejected);
    }
    if (!source.alive) {
        return reject(CombatRejectReason::sourceDead, CombatResult::rejected);
    }
    if (!target.alive) {
        return reject(CombatRejectReason::targetDead, CombatResult::rejected);
    }
    if (!source.definition.canDealDamage || !target.definition.canReceiveDamage) {
        return reject(CombatRejectReason::combatDisabled, CombatResult::rejected);
    }
    const std::uint64_t targetFaction = std::uint64_t{1} << target.definition.faction;
    if (source.definition.bubble != target.definition.bubble
        || (source.definition.hostileFactionMask & targetFaction) == 0) {
        return reject(CombatRejectReason::notHostile, CombatResult::rejected);
    }
    if (intent.command.executionTick < source.nextDamageTick) {
        return reject(CombatRejectReason::cooldown, CombatResult::rejected);
    }
    if (intent.claimedTick > intent.command.executionTick
        || intent.command.executionTick - intent.claimedTick >= kCombatRewindSampleCapacity) {
        return reject(CombatRejectReason::claimedTick, CombatResult::rejected);
    }
    CombatPoseSample sourcePose{};
    CombatPoseSample targetPose{};
    if (!internal::find_pose(source, intent.claimedTick, sourcePose)
        || !internal::find_pose(target, intent.claimedTick, targetPose)) {
        return reject(CombatRejectReason::rewindMissing, CombatResult::rejected);
    }
    if (source.authorityEpoch == kAbsentGeneration
        || sourcePose.authorityEpoch != source.authorityEpoch
        || targetPose.authorityEpoch != target.authorityEpoch
        || intent.hitEvidence.sourceAuthorityEpoch != source.authorityEpoch) {
        return reject(CombatRejectReason::authority, CombatResult::rejected);
    }
    if (!internal::within_range(
            sourcePose.position, targetPose.position, attack.definition.maximumRangeMillimeters)) {
        return reject(CombatRejectReason::range, CombatResult::rejected);
    }
    if (!intent.hitEvidence.lineOfSight) {
        return reject(CombatRejectReason::lineOfSight, CombatResult::rejected);
    }
    if (source.revision == std::numeric_limits<std::uint64_t>::max()
        || target.revision == std::numeric_limits<std::uint64_t>::max()) {
        return reject(CombatRejectReason::capacity, CombatResult::rejected);
    }

    const std::uint32_t oldHealth = target.health;
    const std::uint32_t oldShield = target.shield;
    std::uint32_t remainingDamage = attack.definition.damage;
    const std::uint32_t shieldDamage = std::min(target.shield, remainingDamage);
    const std::uint32_t newShield = target.shield - shieldDamage;
    remainingDamage -= shieldDamage;
    const std::uint32_t healthDamage = std::min(target.health, remainingDamage);
    const std::uint32_t newHealth = target.health - healthDamage;
    const bool died = newHealth == 0;
    const std::size_t requiredEvents = died ? 3U : 2U;
    if (events.size() < requiredEvents) {
        return CombatResult::outputCapacity;
    }
    if (commands_.remember(intent.command) != DeduplicationResult::recorded) {
        return reject(CombatRejectReason::capacity, CombatResult::rejected);
    }

    source.nextDamageTick = internal::saturating_tick_add(intent.command.executionTick,
                                                          attack.definition.cooldownTicks);
    ++source.revision;
    target.health = newHealth;
    target.shield = newShield;
    target.alive = !died;
    ++target.revision;
    const CombatEvent base{intent.sourceActor,
                           intent.targetActor,
                           worldEpoch_,
                           intent.command.executionTick,
                           intent.command.commandId,
                           attack.definition.attackBlueprintId,
                           source.revision,
                           target.revision,
                           oldHealth,
                           newHealth,
                           oldShield,
                           newShield,
                           CombatEventKind::damageAccepted,
                           CombatRejectReason::none};
    events[0] = base;
    events[1] = base;
    events[1].kind = CombatEventKind::healthChanged;
    if (died) {
        events[2] = base;
        events[2].kind = CombatEventKind::death;
    }
    eventCount = requiredEvents;
    return CombatResult::applied;
}

} // namespace sunrise::server::gameplay::physics::mechanics
