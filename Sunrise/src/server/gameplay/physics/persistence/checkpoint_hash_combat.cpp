#include "checkpoint_hash_internal.h"

namespace sunrise::server::gameplay::physics::persistence::detail {
namespace {

/** Appends one complete generic mechanics command-deduplication window. */
void hash_commands(CheckpointHashWriter& hash,
                   const mechanics::CommandDeduplicatorSnapshot& snapshot) noexcept {
    hash.scalar(snapshot.count);
    for (const mechanics::CommandRecord& command : snapshot.records) {
        hash.scalar(command.commandId);
        hash.scalar(command.executionTick);
        hash.scalar(command.policyOwnerId);
        hash.scalar(command.occupied);
    }
}

/** Appends one exact actor identity. */
void hash_actor(CheckpointHashWriter& hash, mechanics::ActorHandle actor) noexcept {
    hash.scalar(actor.actorId);
    hash.scalar(actor.generation);
}

/** Appends one retained rewind pose without padding bytes. */
void hash_pose(CheckpointHashWriter& hash, const mechanics::CombatPoseSample& pose) noexcept {
    hash.scalar(pose.position.x);
    hash.scalar(pose.position.y);
    hash.scalar(pose.position.z);
    hash.scalar(pose.tick);
    hash.scalar(pose.authorityEpoch);
}

} // namespace

/** Appends every combat definition, live value, rewind pose, and attack. */
void hash_combat(CheckpointHashWriter& hash,
                 const mechanics::CombatKernelSnapshot& snapshot) noexcept {
    hash.scalar(snapshot.worldEpoch);
    for (const mechanics::CombatantSnapshot& combatant : snapshot.combatants) {
        hash_actor(hash, combatant.definition.actor);
        hash.scalar(combatant.definition.policyOwnerId);
        hash.scalar(combatant.definition.hostileFactionMask);
        hash.scalar(combatant.definition.bubble);
        hash.scalar(combatant.definition.maximumHealth);
        hash.scalar(combatant.definition.maximumShield);
        hash.scalar(combatant.definition.faction);
        hash.scalar(combatant.definition.canDealDamage);
        hash.scalar(combatant.definition.canReceiveDamage);
        for (const mechanics::CombatPoseSample& pose : combatant.rewindSamples) {
            hash_pose(hash, pose);
        }
        hash.scalar(combatant.generation);
        hash.scalar(combatant.revision);
        hash.scalar(combatant.authorityEpoch);
        hash.scalar(combatant.nextDamageTick);
        hash.scalar(combatant.rewindSampleCount);
        hash.scalar(combatant.health);
        hash.scalar(combatant.shield);
        hash.scalar(combatant.active);
        hash.scalar(combatant.alive);
    }
    for (const mechanics::AttackBlueprintSnapshot& attack : snapshot.attacks) {
        hash.scalar(attack.definition.attackBlueprintId);
        hash.scalar(attack.definition.policyOwnerId);
        hash.scalar(attack.definition.blueprintVersion);
        hash.scalar(attack.definition.damage);
        hash.scalar(attack.definition.maximumRangeMillimeters);
        hash.scalar(attack.definition.cooldownTicks);
        hash.scalar(attack.generation);
        hash.scalar(attack.active);
    }
    hash_commands(hash, snapshot.commands);
}

} // namespace sunrise::server::gameplay::physics::persistence::detail
