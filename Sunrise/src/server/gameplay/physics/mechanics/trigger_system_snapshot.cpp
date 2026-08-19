#include "trigger_system.h"

namespace sunrise::server::gameplay::physics::mechanics {

TriggerSystemSnapshot TriggerSystem::snapshot() const noexcept {
    return TriggerSystemSnapshot{slots_,
                                 memberships_,
                                 commands_.snapshot(),
                                 worldEpoch_,
                                 lastEvaluationTick_,
                                 membershipCount_,
                                 hasEvaluation_};
}

/** Replaces trigger state only after every slot and membership validates. */
bool TriggerSystem::restore(const TriggerSystemSnapshot& snapshot) noexcept {
    if (snapshot.worldEpoch == kAbsentWorldEpoch
        || snapshot.membershipCount > snapshot.memberships.size()
        || (!snapshot.hasEvaluation
            && (snapshot.lastEvaluationTick != 0 || snapshot.membershipCount != 0))) {
        return false;
    }
    for (const TriggerSlotSnapshot& slot : snapshot.slots) {
        if (slot.generation == kAbsentGeneration) {
            return false;
        }
        if (slot.active
            && (slot.triggerId == 0 || slot.policyOwnerId == kAbsentPolicyOwnerId
                || slot.createCommandId == kAbsentCommandId)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < snapshot.membershipCount; ++index) {
        const TriggerOccupancy& item = snapshot.memberships[index];
        if (item.trigger.slot >= snapshot.slots.size() || !snapshot.slots[item.trigger.slot].active
            || snapshot.slots[item.trigger.slot].generation != item.trigger.generation
            || !valid_actor_handle(item.actor)) {
            return false;
        }
        if (index != 0 && !occupancy_less(snapshot.memberships[index - 1], item)) {
            return false;
        }
    }
    CommandDeduplicator restoredCommands{};
    if (!restoredCommands.restore(snapshot.commands)) {
        return false;
    }
    slots_ = snapshot.slots;
    memberships_ = snapshot.memberships;
    commands_ = restoredCommands;
    worldEpoch_ = snapshot.worldEpoch;
    lastEvaluationTick_ = snapshot.lastEvaluationTick;
    membershipCount_ = snapshot.membershipCount;
    hasEvaluation_ = snapshot.hasEvaluation;
    return true;
}

bool TriggerSystem::valid_handle(TriggerHandle handle) const noexcept {
    return handle.slot < slots_.size() && handle.generation != kAbsentGeneration
           && slots_[handle.slot].active && slots_[handle.slot].generation == handle.generation;
}

/** Removes every retained membership for one trigger generation. */
void TriggerSystem::erase_memberships(TriggerHandle handle) noexcept {
    std::size_t write = 0;
    for (std::size_t read = 0; read < membershipCount_; ++read) {
        if (!equal_trigger_handle(memberships_[read].trigger, handle)) {
            memberships_[write] = memberships_[read];
            ++write;
        }
    }
    for (std::size_t index = write; index < membershipCount_; ++index) {
        memberships_[index] = {};
    }
    membershipCount_ = write;
}

} // namespace sunrise::server::gameplay::physics::mechanics
