#include "objective_ledger.h"

#include <limits>

namespace sunrise::server::gameplay::physics::mechanics {

namespace {

[[nodiscard]] bool
checked_add(std::int64_t left, std::int64_t right, std::int64_t& value) noexcept {
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right)
        || (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
        return false;
    }
    value = left + right;
    return true;
}

} // namespace

/** @return True when one counter definition is sorted and bounded. */
bool ObjectiveLedger::valid_definition(const ObjectiveCounterDefinition& definition) noexcept {
    if (definition.counterId == 0 || definition.policyOwnerId == kAbsentPolicyOwnerId
        || definition.minimumValue > definition.maximumValue
        || definition.initialValue < definition.minimumValue
        || definition.initialValue > definition.maximumValue
        || definition.thresholdCount > definition.thresholds.size()
        || (definition.policy != ObjectivePolicy::monotonic
            && definition.policy != ObjectivePolicy::bounded)) {
        return false;
    }
    for (std::size_t index = 0; index < definition.thresholdCount; ++index) {
        const std::int64_t threshold = definition.thresholds[index];
        if (threshold < definition.minimumValue || threshold > definition.maximumValue
            || (index != 0 && threshold <= definition.thresholds[index - 1])) {
            return false;
        }
    }
    return true;
}

void ObjectiveLedger::reset(WorldEpoch worldEpoch) noexcept {
    for (ObjectiveCounterSnapshot& counter : counters_) {
        const std::uint64_t generation = next_generation(counter.generation);
        const std::uint64_t exhausted = counter.generation;
        counter = {};
        counter.generation = generation == kAbsentGeneration ? exhausted : generation;
    }
    commands_.clear();
    worldEpoch_ = worldEpoch;
}

/** Creates one policy-owned monotonic or bounded counter. */
ObjectiveResult ObjectiveLedger::create_counter(const ObjectiveCounterDefinition& definition,
                                                ObjectiveCounterHandle& handle) noexcept {
    handle = {};
    if (worldEpoch_ == kAbsentWorldEpoch || !valid_definition(definition)) {
        return ObjectiveResult::invalidDefinition;
    }
    std::size_t freeIndex = counters_.size();
    for (std::size_t index = 0; index < counters_.size(); ++index) {
        const ObjectiveCounterSnapshot& counter = counters_[index];
        if (counter.active && counter.definition.policyOwnerId == definition.policyOwnerId
            && counter.definition.counterId == definition.counterId) {
            return ObjectiveResult::invalidDefinition;
        }
        if (!counter.active && freeIndex == counters_.size()
            && next_generation(counter.generation) != kAbsentGeneration) {
            freeIndex = index;
        }
    }
    if (freeIndex == counters_.size()) {
        return ObjectiveResult::capacity;
    }
    ObjectiveCounterSnapshot& counter = counters_[freeIndex];
    counter.definition = definition;
    counter.generation = next_generation(counter.generation);
    counter.revision = 1;
    counter.value = definition.initialValue;
    counter.active = true;
    handle = ObjectiveCounterHandle{counter.generation, static_cast<std::uint16_t>(freeIndex)};
    return ObjectiveResult::applied;
}

ObjectiveResult ObjectiveLedger::remove_counter(ObjectiveCounterHandle handle) noexcept {
    if (!valid_handle(handle)) {
        return ObjectiveResult::invalidHandle;
    }
    counters_[handle.slot].active = false;
    return ObjectiveResult::applied;
}

/** Applies one idempotent counter mutation and copied threshold events. */
ObjectiveResult ObjectiveLedger::apply(const ObjectiveMutation& mutation,
                                       std::span<ObjectiveEvent> events,
                                       std::size_t& eventCount) noexcept {
    eventCount = 0;
    if (worldEpoch_ == kAbsentWorldEpoch || mutation.command.worldEpoch != worldEpoch_) {
        return ObjectiveResult::staleWorld;
    }
    if (!valid_command_header(mutation.command, worldEpoch_)) {
        return ObjectiveResult::invalidCommand;
    }
    if (mutation.operation != ObjectiveOperation::add
        && mutation.operation != ObjectiveOperation::set) {
        return ObjectiveResult::invalidCommand;
    }
    if (commands_.contains(mutation.command.policyOwnerId, mutation.command.commandId)) {
        return ObjectiveResult::duplicate;
    }
    if (!valid_handle(mutation.counter)) {
        return ObjectiveResult::invalidHandle;
    }
    ObjectiveCounterSnapshot& counter = counters_[mutation.counter.slot];
    if (counter.definition.policyOwnerId != mutation.command.policyOwnerId) {
        return ObjectiveResult::invalidHandle;
    }

    std::int64_t newValue = mutation.value;
    if (mutation.operation == ObjectiveOperation::add
        && !checked_add(counter.value, mutation.value, newValue)) {
        return ObjectiveResult::arithmeticOverflow;
    }
    if (newValue < counter.definition.minimumValue || newValue > counter.definition.maximumValue) {
        return ObjectiveResult::boundViolation;
    }
    if (counter.definition.policy == ObjectivePolicy::monotonic && newValue < counter.value) {
        return ObjectiveResult::monotonicViolation;
    }
    if (newValue == counter.value) {
        if (commands_.remember(mutation.command) != DeduplicationResult::recorded) {
            return ObjectiveResult::capacity;
        }
        return ObjectiveResult::noChange;
    }
    if (counter.revision == std::numeric_limits<std::uint64_t>::max()) {
        return ObjectiveResult::revisionExhausted;
    }

    std::array<ObjectiveEvent, kObjectiveThresholdCapacity + 1> pending{};
    const std::uint64_t newRevision = counter.revision + 1;
    pending[eventCount] = ObjectiveEvent{mutation.counter,
                                         worldEpoch_,
                                         mutation.command.executionTick,
                                         counter.definition.counterId,
                                         counter.definition.policyOwnerId,
                                         newRevision,
                                         counter.value,
                                         newValue,
                                         0,
                                         ObjectiveEventKind::valueChanged};
    ++eventCount;
    for (std::size_t index = 0; index < counter.definition.thresholdCount; ++index) {
        const std::int64_t threshold = counter.definition.thresholds[index];
        ObjectiveEventKind kind{};
        if (counter.value < threshold && newValue >= threshold) {
            kind = ObjectiveEventKind::thresholdCrossedUp;
        } else if (counter.value >= threshold && newValue < threshold) {
            kind = ObjectiveEventKind::thresholdCrossedDown;
        } else {
            continue;
        }
        pending[eventCount] = ObjectiveEvent{mutation.counter,
                                             worldEpoch_,
                                             mutation.command.executionTick,
                                             counter.definition.counterId,
                                             counter.definition.policyOwnerId,
                                             newRevision,
                                             counter.value,
                                             newValue,
                                             threshold,
                                             kind};
        ++eventCount;
    }
    if (events.size() < eventCount) {
        eventCount = 0;
        return ObjectiveResult::outputCapacity;
    }
    if (commands_.remember(mutation.command) != DeduplicationResult::recorded) {
        eventCount = 0;
        return ObjectiveResult::capacity;
    }
    counter.value = newValue;
    counter.revision = newRevision;
    for (std::size_t index = 0; index < eventCount; ++index) {
        events[index] = pending[index];
    }
    return ObjectiveResult::applied;
}

void ObjectiveLedger::expire_commands_before(TickId firstRetainedTick) noexcept {
    commands_.expire_before(firstRetainedTick);
}

} // namespace sunrise::server::gameplay::physics::mechanics
