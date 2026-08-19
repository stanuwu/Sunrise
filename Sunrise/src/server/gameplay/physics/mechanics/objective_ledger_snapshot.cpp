#include "objective_ledger.h"

namespace sunrise::server::gameplay::physics::mechanics {

bool ObjectiveLedger::query(ObjectiveCounterHandle handle,
                            ObjectiveCounterSnapshot& counter) const noexcept {
    if (!valid_handle(handle)) {
        return false;
    }
    counter = counters_[handle.slot];
    return true;
}

ObjectiveLedgerSnapshot ObjectiveLedger::snapshot() const noexcept {
    return ObjectiveLedgerSnapshot{counters_, commands_.snapshot(), worldEpoch_};
}

/** Replaces objective state only after every counter validates. */
bool ObjectiveLedger::restore(const ObjectiveLedgerSnapshot& snapshot) noexcept {
    if (snapshot.worldEpoch == kAbsentWorldEpoch) {
        return false;
    }
    for (std::size_t left = 0; left < snapshot.counters.size(); ++left) {
        const ObjectiveCounterSnapshot& counter = snapshot.counters[left];
        if (counter.generation == kAbsentGeneration) {
            return false;
        }
        if (!counter.active) {
            continue;
        }
        if (!valid_definition(counter.definition) || counter.revision == 0
            || counter.value < counter.definition.minimumValue
            || counter.value > counter.definition.maximumValue
            || (counter.definition.policy == ObjectivePolicy::monotonic
                && counter.value < counter.definition.initialValue)) {
            return false;
        }
        for (std::size_t right = left + 1; right < snapshot.counters.size(); ++right) {
            const ObjectiveCounterSnapshot& other = snapshot.counters[right];
            if (other.active && counter.definition.policyOwnerId == other.definition.policyOwnerId
                && counter.definition.counterId == other.definition.counterId) {
                return false;
            }
        }
    }
    CommandDeduplicator restoredCommands{};
    if (!restoredCommands.restore(snapshot.commands)) {
        return false;
    }
    counters_ = snapshot.counters;
    commands_ = restoredCommands;
    worldEpoch_ = snapshot.worldEpoch;
    return true;
}

bool ObjectiveLedger::valid_handle(ObjectiveCounterHandle handle) const noexcept {
    return handle.slot < counters_.size() && handle.generation != kAbsentGeneration
           && counters_[handle.slot].active
           && counters_[handle.slot].generation == handle.generation;
}

} // namespace sunrise::server::gameplay::physics::mechanics
