#include "combat_kernel.h"
#include "combat_kernel_internal.h"

namespace sunrise::server::gameplay::physics::mechanics {

CombatKernelSnapshot CombatKernel::snapshot() const noexcept {
    return CombatKernelSnapshot{combatants_, attacks_, commands_.snapshot(), worldEpoch_};
}

/** Replaces combat state only after the full snapshot validates. */
bool CombatKernel::restore(const CombatKernelSnapshot& snapshot) noexcept {
    if (snapshot.worldEpoch == kAbsentWorldEpoch) {
        return false;
    }
    for (std::size_t left = 0; left < snapshot.combatants.size(); ++left) {
        const CombatantSnapshot& combatant = snapshot.combatants[left];
        if (combatant.generation == kAbsentGeneration) {
            return false;
        }
        if (!combatant.active) {
            continue;
        }
        if (!internal::valid_combatant_definition(combatant.definition) || combatant.revision == 0
            || combatant.health > combatant.definition.maximumHealth
            || combatant.shield > combatant.definition.maximumShield
            || combatant.alive != (combatant.health != 0)
            || combatant.rewindSampleCount > combatant.rewindSamples.size()) {
            return false;
        }
        for (std::size_t index = 0; index < combatant.rewindSampleCount; ++index) {
            const CombatPoseSample& sample = combatant.rewindSamples[index];
            if (!internal::valid_position(sample.position)
                || sample.authorityEpoch == kAbsentGeneration
                || sample.authorityEpoch != combatant.authorityEpoch
                || (index != 0 && sample.tick <= combatant.rewindSamples[index - 1].tick)) {
                return false;
            }
        }
        for (std::size_t right = left + 1; right < snapshot.combatants.size(); ++right) {
            if (snapshot.combatants[right].active
                && combatant.definition.actor.actorId
                       == snapshot.combatants[right].definition.actor.actorId) {
                return false;
            }
        }
    }
    for (std::size_t left = 0; left < snapshot.attacks.size(); ++left) {
        const AttackBlueprintSnapshot& attack = snapshot.attacks[left];
        if (attack.generation == kAbsentGeneration) {
            return false;
        }
        if (!attack.active) {
            continue;
        }
        if (!internal::valid_attack_definition(attack.definition)) {
            return false;
        }
        for (std::size_t right = left + 1; right < snapshot.attacks.size(); ++right) {
            const AttackBlueprintSnapshot& other = snapshot.attacks[right];
            if (other.active && attack.definition.policyOwnerId == other.definition.policyOwnerId
                && attack.definition.attackBlueprintId == other.definition.attackBlueprintId) {
                return false;
            }
        }
    }
    CommandDeduplicator restoredCommands{};
    if (!restoredCommands.restore(snapshot.commands)) {
        return false;
    }
    combatants_ = snapshot.combatants;
    attacks_ = snapshot.attacks;
    commands_ = restoredCommands;
    worldEpoch_ = snapshot.worldEpoch;
    return true;
}

} // namespace sunrise::server::gameplay::physics::mechanics
