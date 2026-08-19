#include "tick_runtime.h"

namespace sunrise::server::gameplay::physics::host::detail {
namespace {

[[nodiscard]] const BubbleHost::Storage::TriggerBinding*
find_trigger(const BubbleHost::Storage::WorldSlot& slot, backend::BodyHandle body) noexcept {
    for (const BubbleHost::Storage::TriggerBinding& binding : slot.triggerBindings) {
        if (binding.occupied && binding.body == body) {
            return &binding;
        }
    }
    return nullptr;
}

[[nodiscard]] const BubbleHost::Storage::BodyBinding*
find_actor(const BubbleHost::Storage::WorldSlot& slot, backend::BodyHandle body) noexcept {
    for (const BubbleHost::Storage::BodyBinding& binding : slot.bodies) {
        if (binding.occupied && binding.body == body) {
            return &binding;
        }
    }
    return nullptr;
}

[[nodiscard]] bool same_occupancy(const mechanics::TriggerOccupancy& left,
                                  const mechanics::TriggerOccupancy& right) noexcept {
    return left.trigger.slot == right.trigger.slot
           && left.trigger.generation == right.trigger.generation
           && mechanics::equal_actor_handle(left.actor, right.actor);
}

/** Appends one exact trigger and actor occupancy pair. */
[[nodiscard]] bool append_occupancy(
    std::array<mechanics::TriggerOccupancy, mechanics::kTriggerMembershipCapacity>& occupancy,
    std::size_t& count,
    const BubbleHost::Storage::TriggerBinding* trigger,
    const BubbleHost::Storage::BodyBinding* actor) noexcept {
    if (trigger == nullptr || actor == nullptr) {
        return true;
    }
    mechanics::TriggerOccupancy candidate{};
    candidate.trigger = trigger->trigger;
    candidate.actor = {actor->actor.id, actor->actor.generation};
    for (std::size_t index = 0; index < count; ++index) {
        if (same_occupancy(occupancy[index], candidate)) {
            return true;
        }
    }
    if (count == occupancy.size()) {
        return false;
    }
    occupancy[count++] = candidate;
    return true;
}

[[nodiscard]] world::CommittedEvent trigger_event(const mechanics::TriggerEvent& source) noexcept {
    world::CommittedEvent event{};
    event.actor = {source.actor.actorId, source.actor.generation};
    event.tick = source.tick;
    event.actorRevision = static_cast<std::uint64_t>(source.edge);
    event.value = source.triggerId;
    event.kind = world::CommittedEventKind::serviceTickCommitted;
    return event;
}

} // namespace

/** Converts trigger-body contacts into stable enter, stay, and leave edges. */
bool run_triggers(BubbleHost::Storage::WorldSlot& slot,
                  const world::HostExecutionContext& context,
                  EventWriter& writer) noexcept {
    std::array<mechanics::TriggerOccupancy, mechanics::kTriggerMembershipCapacity> occupancy{};
    std::size_t occupancyCount = 0;
    for (std::size_t index = 0; index < slot.contacts.count; ++index) {
        const backend::Contact& contact = slot.contacts.contacts[index];
        if (!append_occupancy(occupancy,
                              occupancyCount,
                              find_trigger(slot, contact.firstBody),
                              find_actor(slot, contact.secondBody))
            || !append_occupancy(occupancy,
                                 occupancyCount,
                                 find_trigger(slot, contact.secondBody),
                                 find_actor(slot, contact.firstBody))) {
            return false;
        }
    }
    slot.triggerEvents = {};
    slot.triggerEventCount = 0;
    const mechanics::TriggerResult result =
        slot.triggers.evaluate(context.tick,
                               std::span(occupancy).first(occupancyCount),
                               slot.triggerEvents,
                               slot.triggerEventCount);
    if (result != mechanics::TriggerResult::applied
        && result != mechanics::TriggerResult::noChange) {
        return false;
    }
    for (std::size_t index = 0; index < slot.triggerEventCount; ++index) {
        if (!writer.append(trigger_event(slot.triggerEvents[index]))) {
            return false;
        }
    }
    return true;
}

} // namespace sunrise::server::gameplay::physics::host::detail
