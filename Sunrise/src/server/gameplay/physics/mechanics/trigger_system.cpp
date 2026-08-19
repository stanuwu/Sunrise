#include "trigger_system.h"

#include <algorithm>

namespace sunrise::server::gameplay::physics::mechanics {

bool TriggerSystem::equal_trigger_handle(TriggerHandle left, TriggerHandle right) noexcept {
    return left.slot == right.slot && left.generation == right.generation;
}

/** @return Stable order for two trigger occupancies. */
bool TriggerSystem::occupancy_less(const TriggerOccupancy& left,
                                   const TriggerOccupancy& right) noexcept {
    if (left.trigger.slot != right.trigger.slot) {
        return left.trigger.slot < right.trigger.slot;
    }
    if (left.trigger.generation != right.trigger.generation) {
        return left.trigger.generation < right.trigger.generation;
    }
    if (left.actor.actorId != right.actor.actorId) {
        return left.actor.actorId < right.actor.actorId;
    }
    return left.actor.generation < right.actor.generation;
}

bool TriggerSystem::equal_occupancy(const TriggerOccupancy& left,
                                    const TriggerOccupancy& right) noexcept {
    return equal_trigger_handle(left.trigger, right.trigger)
           && equal_actor_handle(left.actor, right.actor);
}

/** Clears triggers and preserves exhausted generation markers. */
void TriggerSystem::reset(WorldEpoch worldEpoch) noexcept {
    for (TriggerSlotSnapshot& slot : slots_) {
        const std::uint64_t generation = next_generation(slot.generation);
        const std::uint64_t exhausted = slot.generation;
        slot = {};
        slot.generation = generation == kAbsentGeneration ? exhausted : generation;
    }
    memberships_ = {};
    commands_.clear();
    worldEpoch_ = worldEpoch;
    lastEvaluationTick_ = 0;
    membershipCount_ = 0;
    hasEvaluation_ = false;
}

/** Creates one policy-owned logical trigger. */
TriggerResult TriggerSystem::create(const CommandHeader& command,
                                    std::uint64_t triggerId,
                                    std::uint32_t bubble,
                                    TriggerHandle& handle) noexcept {
    handle = {};
    if (worldEpoch_ == kAbsentWorldEpoch || command.worldEpoch != worldEpoch_) {
        return TriggerResult::staleWorld;
    }
    if (!valid_command_header(command, worldEpoch_)) {
        return TriggerResult::invalidCommand;
    }
    if (triggerId == 0) {
        return TriggerResult::invalidDefinition;
    }
    if (commands_.contains(command.policyOwnerId, command.commandId)) {
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            const TriggerSlotSnapshot& slot = slots_[index];
            if (slot.active && slot.policyOwnerId == command.policyOwnerId
                && slot.createCommandId == command.commandId) {
                handle = TriggerHandle{slot.generation, static_cast<std::uint16_t>(index)};
                break;
            }
        }
        return TriggerResult::duplicate;
    }
    std::size_t freeIndex = slots_.size();
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        const TriggerSlotSnapshot& slot = slots_[index];
        if (slot.active && slot.policyOwnerId == command.policyOwnerId
            && slot.triggerId == triggerId) {
            return TriggerResult::invalidDefinition;
        }
        if (!slot.active && freeIndex == slots_.size()
            && next_generation(slot.generation) != kAbsentGeneration) {
            freeIndex = index;
        }
    }
    if (freeIndex == slots_.size()) {
        return TriggerResult::capacity;
    }
    if (commands_.remember(command) != DeduplicationResult::recorded) {
        return TriggerResult::capacity;
    }
    TriggerSlotSnapshot& slot = slots_[freeIndex];
    slot.generation = next_generation(slot.generation);
    slot.triggerId = triggerId;
    slot.policyOwnerId = command.policyOwnerId;
    slot.createCommandId = command.commandId;
    slot.bubble = bubble;
    slot.active = true;
    handle = TriggerHandle{slot.generation, static_cast<std::uint16_t>(freeIndex)};
    return TriggerResult::applied;
}

/** Removes one exact trigger and all of its retained occupancy. */
TriggerResult TriggerSystem::remove(const CommandHeader& command, TriggerHandle handle) noexcept {
    if (worldEpoch_ == kAbsentWorldEpoch || command.worldEpoch != worldEpoch_) {
        return TriggerResult::staleWorld;
    }
    if (!valid_command_header(command, worldEpoch_)) {
        return TriggerResult::invalidCommand;
    }
    if (commands_.contains(command.policyOwnerId, command.commandId)) {
        return TriggerResult::duplicate;
    }
    if (!valid_handle(handle) || slots_[handle.slot].policyOwnerId != command.policyOwnerId) {
        return TriggerResult::invalidHandle;
    }
    if (commands_.remember(command) != DeduplicationResult::recorded) {
        return TriggerResult::capacity;
    }
    slots_[handle.slot].active = false;
    erase_memberships(handle);
    return TriggerResult::applied;
}

/** Converts a complete occupancy set into deterministic edge events. */
TriggerResult TriggerSystem::evaluate(TickId tick,
                                      std::span<const TriggerOccupancy> occupancy,
                                      std::span<TriggerEvent> events,
                                      std::size_t& eventCount) noexcept {
    eventCount = 0;
    bool hasActiveTrigger = false;
    for (const TriggerSlotSnapshot& slot : slots_) {
        hasActiveTrigger = hasActiveTrigger || slot.active;
    }
    if (!hasActiveTrigger) {
        return TriggerResult::noChange;
    }
    if (hasEvaluation_ && tick <= lastEvaluationTick_) {
        return TriggerResult::invalidTick;
    }
    if (occupancy.size() > kTriggerMembershipCapacity) {
        return TriggerResult::capacity;
    }
    std::array<TriggerOccupancy, kTriggerMembershipCapacity> next{};
    std::size_t nextCount = 0;
    for (const TriggerOccupancy& item : occupancy) {
        if (!valid_handle(item.trigger) || !valid_actor_handle(item.actor)) {
            return TriggerResult::invalidOccupancy;
        }
        next[nextCount] = item;
        ++nextCount;
    }
    std::sort(next.begin(), next.begin() + static_cast<std::ptrdiff_t>(nextCount), occupancy_less);
    for (std::size_t index = 1; index < nextCount; ++index) {
        if (equal_occupancy(next[index - 1], next[index])) {
            return TriggerResult::invalidOccupancy;
        }
    }

    std::array<TriggerEvent, kTriggerMembershipCapacity * 2> pending{};
    std::size_t oldIndex = 0;
    std::size_t nextIndex = 0;
    while (oldIndex < membershipCount_ || nextIndex < nextCount) {
        const bool hasOld = oldIndex < membershipCount_;
        const bool hasNext = nextIndex < nextCount;
        const bool same =
            hasOld && hasNext && equal_occupancy(memberships_[oldIndex], next[nextIndex]);
        const bool takeOld =
            hasOld && (!hasNext || occupancy_less(memberships_[oldIndex], next[nextIndex]));
        const TriggerOccupancy& selected =
            same || !takeOld ? next[nextIndex] : memberships_[oldIndex];
        const TriggerEdge edge =
            same ? TriggerEdge::stay : (takeOld ? TriggerEdge::leave : TriggerEdge::enter);
        const TriggerSlotSnapshot& trigger = slots_[selected.trigger.slot];
        pending[eventCount] = TriggerEvent{selected.trigger,
                                           selected.actor,
                                           worldEpoch_,
                                           tick,
                                           trigger.triggerId,
                                           trigger.policyOwnerId,
                                           trigger.bubble,
                                           edge};
        ++eventCount;
        oldIndex += same || takeOld ? 1U : 0U;
        nextIndex += same || !takeOld ? 1U : 0U;
    }
    if (events.size() < eventCount) {
        eventCount = 0;
        return TriggerResult::outputCapacity;
    }
    for (std::size_t index = 0; index < eventCount; ++index) {
        events[index] = pending[index];
    }
    memberships_ = next;
    membershipCount_ = nextCount;
    lastEvaluationTick_ = tick;
    hasEvaluation_ = true;
    return eventCount == 0 ? TriggerResult::noChange : TriggerResult::applied;
}

void TriggerSystem::expire_commands_before(TickId firstRetainedTick) noexcept {
    commands_.expire_before(firstRetainedTick);
}

} // namespace sunrise::server::gameplay::physics::mechanics
