#include "tick_timer.h"

#include <algorithm>

namespace sunrise::server::gameplay::physics::mechanics {

namespace {

/** @return Stable fire order for two timer events. */
[[nodiscard]] bool event_less(const TickTimerEvent& left, const TickTimerEvent& right) noexcept {
    if (left.scheduledTick != right.scheduledTick) {
        return left.scheduledTick < right.scheduledTick;
    }
    if (left.policyOwnerId != right.policyOwnerId) {
        return left.policyOwnerId < right.policyOwnerId;
    }
    if (left.timerId != right.timerId) {
        return left.timerId < right.timerId;
    }
    return left.timer.slot < right.timer.slot;
}

} // namespace

void TickTimerService::reset(WorldEpoch worldEpoch) noexcept {
    for (TickTimerSlotSnapshot& slot : slots_) {
        const std::uint64_t generation = next_generation(slot.generation);
        const std::uint64_t exhausted = slot.generation;
        slot = {};
        slot.generation = generation == kAbsentGeneration ? exhausted : generation;
    }
    commands_.clear();
    worldEpoch_ = worldEpoch;
}

/** Schedules one future timer without reusing exhausted slots. */
TickTimerResult TickTimerService::schedule(const CommandHeader& command,
                                           std::uint64_t timerId,
                                           TickId fireTick,
                                           TickTimerHandle& handle) noexcept {
    handle = {};
    if (worldEpoch_ == kAbsentWorldEpoch || command.worldEpoch != worldEpoch_) {
        return TickTimerResult::staleWorld;
    }
    if (!valid_command_header(command, worldEpoch_) || timerId == 0) {
        return TickTimerResult::invalidCommand;
    }
    if (fireTick < command.executionTick) {
        return TickTimerResult::invalidTick;
    }
    if (commands_.contains(command.policyOwnerId, command.commandId)) {
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            const TickTimerSlotSnapshot& slot = slots_[index];
            if (slot.active && slot.policyOwnerId == command.policyOwnerId
                && slot.scheduleCommandId == command.commandId) {
                handle = TickTimerHandle{slot.generation, static_cast<std::uint16_t>(index)};
                break;
            }
        }
        return TickTimerResult::duplicate;
    }
    std::size_t freeIndex = slots_.size();
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        if (!slots_[index].active
            && next_generation(slots_[index].generation) != kAbsentGeneration) {
            freeIndex = index;
            break;
        }
    }
    if (freeIndex == slots_.size()) {
        return TickTimerResult::capacity;
    }
    if (commands_.remember(command) != DeduplicationResult::recorded) {
        return TickTimerResult::capacity;
    }
    TickTimerSlotSnapshot& slot = slots_[freeIndex];
    slot.generation = next_generation(slot.generation);
    slot.timerId = timerId;
    slot.fireTick = fireTick;
    slot.policyOwnerId = command.policyOwnerId;
    slot.scheduleCommandId = command.commandId;
    slot.active = true;
    handle = TickTimerHandle{slot.generation, static_cast<std::uint16_t>(freeIndex)};
    return TickTimerResult::applied;
}

/** Cancels one exact timer generation idempotently. */
TickTimerResult TickTimerService::cancel(const CommandHeader& command,
                                         TickTimerHandle handle) noexcept {
    if (worldEpoch_ == kAbsentWorldEpoch || command.worldEpoch != worldEpoch_) {
        return TickTimerResult::staleWorld;
    }
    if (!valid_command_header(command, worldEpoch_)) {
        return TickTimerResult::invalidCommand;
    }
    if (commands_.contains(command.policyOwnerId, command.commandId)) {
        return TickTimerResult::duplicate;
    }
    if (!valid_handle(handle)) {
        return TickTimerResult::invalidHandle;
    }
    if (slots_[handle.slot].policyOwnerId != command.policyOwnerId) {
        return TickTimerResult::invalidCommand;
    }
    if (commands_.remember(command) != DeduplicationResult::recorded) {
        return TickTimerResult::capacity;
    }
    slots_[handle.slot].active = false;
    return TickTimerResult::applied;
}

/** Fires every due timer in stable order with atomic output. */
TickTimerResult TickTimerService::fire_due(TickId tick,
                                           std::span<TickTimerEvent> events,
                                           std::size_t& eventCount) noexcept {
    eventCount = 0;
    if (worldEpoch_ == kAbsentWorldEpoch) {
        return TickTimerResult::noChange;
    }
    std::array<TickTimerEvent, kTickTimerCapacity> pending{};
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        const TickTimerSlotSnapshot& slot = slots_[index];
        if (!slot.active || slot.fireTick > tick) {
            continue;
        }
        pending[eventCount] = TickTimerEvent{
            TickTimerHandle{slot.generation, static_cast<std::uint16_t>(index)},
            worldEpoch_,
            slot.fireTick,
            tick,
            slot.timerId,
            slot.policyOwnerId,
            slot.scheduleCommandId,
        };
        ++eventCount;
    }
    if (eventCount == 0) {
        return TickTimerResult::noChange;
    }
    if (events.size() < eventCount) {
        eventCount = 0;
        return TickTimerResult::outputCapacity;
    }
    std::sort(
        pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(eventCount), event_less);
    for (std::size_t index = 0; index < eventCount; ++index) {
        events[index] = pending[index];
        slots_[pending[index].timer.slot].active = false;
    }
    return TickTimerResult::applied;
}

void TickTimerService::expire_commands_before(TickId firstRetainedTick) noexcept {
    commands_.expire_before(firstRetainedTick);
}

TickTimerServiceSnapshot TickTimerService::snapshot() const noexcept {
    return TickTimerServiceSnapshot{slots_, commands_.snapshot(), worldEpoch_};
}

/** Replaces timer state only after every retained slot validates. */
bool TickTimerService::restore(const TickTimerServiceSnapshot& snapshot) noexcept {
    if (snapshot.worldEpoch == kAbsentWorldEpoch) {
        return false;
    }
    for (const TickTimerSlotSnapshot& slot : snapshot.slots) {
        if (slot.generation == kAbsentGeneration) {
            return false;
        }
        if (slot.active
            && (slot.timerId == 0 || slot.policyOwnerId == kAbsentPolicyOwnerId
                || slot.scheduleCommandId == kAbsentCommandId)) {
            return false;
        }
    }
    CommandDeduplicator restoredCommands{};
    if (!restoredCommands.restore(snapshot.commands)) {
        return false;
    }
    slots_ = snapshot.slots;
    commands_ = restoredCommands;
    worldEpoch_ = snapshot.worldEpoch;
    return true;
}

bool TickTimerService::valid_handle(TickTimerHandle handle) const noexcept {
    return handle.slot < slots_.size() && handle.generation != kAbsentGeneration
           && slots_[handle.slot].active && slots_[handle.slot].generation == handle.generation;
}

} // namespace sunrise::server::gameplay::physics::mechanics
