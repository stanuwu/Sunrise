#include "combat_kernel.h"

#include <limits>

#include "combat_kernel_internal.h"

namespace sunrise::server::gameplay::physics::mechanics {

/** Clears combat state and preserves exhausted generation markers. */
void CombatKernel::reset(WorldEpoch worldEpoch) noexcept {
    for (CombatantSnapshot& combatant : combatants_) {
        const std::uint64_t generation = next_generation(combatant.generation);
        const std::uint64_t exhausted = combatant.generation;
        combatant = {};
        combatant.generation = generation == kAbsentGeneration ? exhausted : generation;
    }
    for (AttackBlueprintSnapshot& attack : attacks_) {
        const std::uint64_t generation = next_generation(attack.generation);
        const std::uint64_t exhausted = attack.generation;
        attack = {};
        attack.generation = generation == kAbsentGeneration ? exhausted : generation;
    }
    commands_.clear();
    worldEpoch_ = worldEpoch;
}

/** Creates one exact neutral-by-default combatant. */
CombatResult CombatKernel::create_combatant(const CombatantDefinition& definition,
                                            CombatantHandle& handle) noexcept {
    handle = {};
    if (worldEpoch_ == kAbsentWorldEpoch || !internal::valid_combatant_definition(definition)) {
        return CombatResult::invalidDefinition;
    }
    std::size_t freeIndex = combatants_.size();
    for (std::size_t index = 0; index < combatants_.size(); ++index) {
        if (combatants_[index].active
            && combatants_[index].definition.actor.actorId == definition.actor.actorId) {
            return CombatResult::invalidDefinition;
        }
        if (!combatants_[index].active && freeIndex == combatants_.size()
            && next_generation(combatants_[index].generation) != kAbsentGeneration) {
            freeIndex = index;
        }
    }
    if (freeIndex == combatants_.size()) {
        return CombatResult::capacity;
    }
    CombatantSnapshot& combatant = combatants_[freeIndex];
    const std::uint64_t generation = next_generation(combatant.generation);
    combatant = {};
    combatant.definition = definition;
    combatant.generation = generation;
    combatant.revision = 1;
    combatant.health = definition.maximumHealth;
    combatant.shield = definition.maximumShield;
    combatant.active = true;
    combatant.alive = true;
    handle = CombatantHandle{generation, static_cast<std::uint16_t>(freeIndex)};
    return CombatResult::applied;
}

CombatResult CombatKernel::remove_combatant(CombatantHandle handle) noexcept {
    if (!valid_combatant_handle(handle)) {
        return CombatResult::invalidHandle;
    }
    combatants_[handle.slot].active = false;
    return CombatResult::applied;
}

/** Creates one versioned attack after validating every bound. */
CombatResult CombatKernel::create_attack_blueprint(const AttackBlueprintDefinition& definition,
                                                   AttackBlueprintHandle& handle) noexcept {
    handle = {};
    if (worldEpoch_ == kAbsentWorldEpoch || !internal::valid_attack_definition(definition)) {
        return CombatResult::invalidDefinition;
    }
    std::size_t freeIndex = attacks_.size();
    for (std::size_t index = 0; index < attacks_.size(); ++index) {
        const AttackBlueprintSnapshot& attack = attacks_[index];
        if (attack.active && attack.definition.policyOwnerId == definition.policyOwnerId
            && attack.definition.attackBlueprintId == definition.attackBlueprintId) {
            return CombatResult::invalidDefinition;
        }
        if (!attack.active && freeIndex == attacks_.size()
            && next_generation(attack.generation) != kAbsentGeneration) {
            freeIndex = index;
        }
    }
    if (freeIndex == attacks_.size()) {
        return CombatResult::capacity;
    }
    AttackBlueprintSnapshot& attack = attacks_[freeIndex];
    const std::uint64_t generation = next_generation(attack.generation);
    attack = {};
    attack.definition = definition;
    attack.generation = generation;
    attack.active = true;
    handle = AttackBlueprintHandle{generation, static_cast<std::uint16_t>(freeIndex)};
    return CombatResult::applied;
}

CombatResult CombatKernel::remove_attack_blueprint(AttackBlueprintHandle handle) noexcept {
    if (!valid_attack_handle(handle)) {
        return CombatResult::invalidHandle;
    }
    attacks_[handle.slot].active = false;
    return CombatResult::applied;
}

/** Changes one combatant authority and drops incompatible rewind state. */
CombatResult CombatKernel::set_authority(CombatantHandle handle,
                                         std::uint64_t authorityEpoch) noexcept {
    if (!valid_combatant_handle(handle)) {
        return CombatResult::invalidHandle;
    }
    if (authorityEpoch == kAbsentGeneration) {
        return CombatResult::invalidDefinition;
    }
    CombatantSnapshot& combatant = combatants_[handle.slot];
    if (combatant.authorityEpoch == authorityEpoch) {
        return CombatResult::noChange;
    }
    if (combatant.authorityEpoch != kAbsentGeneration
        && authorityEpoch < combatant.authorityEpoch) {
        return CombatResult::invalidDefinition;
    }
    if (combatant.revision == std::numeric_limits<std::uint64_t>::max()) {
        return CombatResult::capacity;
    }
    combatant.authorityEpoch = authorityEpoch;
    combatant.rewindSamples = {};
    combatant.rewindSampleCount = 0;
    ++combatant.revision;
    return CombatResult::applied;
}

/** Retains one ordered canonical pose for later host validation. */
CombatResult CombatKernel::record_pose(CombatantHandle handle,
                                       const CombatPoseSample& sample) noexcept {
    if (!valid_combatant_handle(handle)) {
        return CombatResult::invalidHandle;
    }
    CombatantSnapshot& combatant = combatants_[handle.slot];
    if (sample.authorityEpoch == kAbsentGeneration
        || sample.authorityEpoch != combatant.authorityEpoch
        || !internal::valid_position(sample.position)) {
        return CombatResult::invalidDefinition;
    }
    if (combatant.rewindSampleCount != 0) {
        CombatPoseSample& latest = combatant.rewindSamples[combatant.rewindSampleCount - 1];
        if (sample.tick < latest.tick) {
            return CombatResult::invalidTick;
        }
        if (sample.tick == latest.tick) {
            if (sample.authorityEpoch == latest.authorityEpoch
                && internal::equal_position(sample.position, latest.position)) {
                return CombatResult::noChange;
            }
            latest = sample;
            return CombatResult::applied;
        }
    }
    if (combatant.rewindSampleCount < combatant.rewindSamples.size()) {
        combatant.rewindSamples[combatant.rewindSampleCount] = sample;
        ++combatant.rewindSampleCount;
    } else {
        for (std::size_t index = 1; index < combatant.rewindSamples.size(); ++index) {
            combatant.rewindSamples[index - 1] = combatant.rewindSamples[index];
        }
        combatant.rewindSamples.back() = sample;
    }
    return CombatResult::applied;
}

bool CombatKernel::query(CombatantHandle handle, CombatantSnapshot& combatant) const noexcept {
    if (!valid_combatant_handle(handle)) {
        return false;
    }
    combatant = combatants_[handle.slot];
    return true;
}

bool CombatKernel::valid_combatant_handle(CombatantHandle handle) const noexcept {
    return handle.slot < combatants_.size() && handle.generation != kAbsentGeneration
           && combatants_[handle.slot].active
           && combatants_[handle.slot].generation == handle.generation;
}

bool CombatKernel::valid_attack_handle(AttackBlueprintHandle handle) const noexcept {
    return handle.slot < attacks_.size() && handle.generation != kAbsentGeneration
           && attacks_[handle.slot].active && attacks_[handle.slot].generation == handle.generation;
}

/** @return The slot containing one exact logical actor, or capacity. */
std::size_t CombatKernel::find_actor(ActorHandle actor) const noexcept {
    if (!valid_actor_handle(actor)) {
        return combatants_.size();
    }
    for (std::size_t index = 0; index < combatants_.size(); ++index) {
        if (combatants_[index].active
            && equal_actor_handle(combatants_[index].definition.actor, actor)) {
            return index;
        }
    }
    return combatants_.size();
}

} // namespace sunrise::server::gameplay::physics::mechanics
