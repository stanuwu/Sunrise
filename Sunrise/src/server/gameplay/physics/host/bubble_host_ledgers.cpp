#include <variant>

#include "command_effects.h"

namespace sunrise::server::gameplay::physics::host::detail {
namespace {

[[nodiscard]] mechanics::CommandHeader
mechanics_header(const world::HostExecutionContext& context,
                 const world::HostCommand& command) noexcept {
    return {context.world.worldGeneration,
            command.header.commandId,
            context.tick,
            command.header.policyOwnerId,
            command.header.definition.version};
}

/** Applies one preflighted objective mutation and copies its events. */
[[nodiscard]] bool apply_objective(BubbleHost::Storage::WorldSlot& slot,
                                   const world::HostExecutionContext& context,
                                   const world::HostCommand& command,
                                   EventWriter& writer) noexcept {
    const mechanics::ObjectiveLedgerSnapshot snapshot = slot.objectives.snapshot();
    std::uint64_t counterId = 0;
    std::uint64_t expectedRevision = 0;
    mechanics::ObjectiveMutation mutation{};
    mutation.command = mechanics_header(context, command);
    if (world::command_kind(command) == world::HostCommandKind::addObjectiveCounter) {
        const auto& value = std::get<world::AddObjectiveCounterCommand>(command.payload);
        counterId = value.counterId;
        expectedRevision = value.expectedRevision;
        mutation.value = value.amount;
        mutation.operation = mechanics::ObjectiveOperation::add;
    } else {
        const auto& value = std::get<world::SetObjectiveCounterCommand>(command.payload);
        counterId = value.counterId;
        expectedRevision = value.expectedRevision;
        mutation.value = value.value;
        mutation.operation = mechanics::ObjectiveOperation::set;
    }
    bool found = false;
    for (std::size_t index = 0; index < snapshot.counters.size(); ++index) {
        const mechanics::ObjectiveCounterSnapshot& counter = snapshot.counters[index];
        if (!counter.active || counter.definition.counterId != counterId) {
            continue;
        }
        if (counter.definition.policyOwnerId != command.header.policyOwnerId
            || counter.revision != expectedRevision) {
            return false;
        }
        mutation.counter = {counter.generation, static_cast<std::uint16_t>(index)};
        found = true;
        break;
    }
    if (!found) {
        return false;
    }
    std::array<mechanics::ObjectiveEvent, mechanics::kObjectiveThresholdCapacity + 1> events{};
    std::size_t eventCount = 0;
    const mechanics::ObjectiveResult result = slot.objectives.apply(mutation, events, eventCount);
    if (result != mechanics::ObjectiveResult::applied
        && result != mechanics::ObjectiveResult::noChange) {
        return false;
    }
    for (std::size_t index = 0; index < eventCount; ++index) {
        world::CommittedEvent event{};
        event.commandId = command.header.commandId;
        event.tick = context.tick;
        event.actorRevision = events[index].revision;
        event.value = events[index].counterId;
        event.kind = world::CommittedEventKind::objectiveChanged;
        if (!writer.append(event)) {
            return false;
        }
    }
    return true;
}

/** Applies one preflighted participant credit mutation. */
[[nodiscard]] bool apply_credit(BubbleHost::Storage::WorldSlot& slot,
                                const world::HostExecutionContext& context,
                                const world::HostCommand& command,
                                EventWriter& writer) noexcept {
    const auto& value = std::get<world::RecordParticipantCreditCommand>(command.payload);
    const mechanics::ParticipantCreditLedgerSnapshot snapshot = slot.credits.snapshot();
    mechanics::ParticipantCreditHandle handle{};
    bool found = false;
    for (std::size_t index = 0; index < snapshot.participants.size(); ++index) {
        const mechanics::ParticipantCreditSnapshot& participant = snapshot.participants[index];
        if (!participant.active || participant.definition.participantId != value.participantId) {
            continue;
        }
        if (participant.definition.policyOwnerId != command.header.policyOwnerId) {
            return false;
        }
        handle = {participant.generation, static_cast<std::uint16_t>(index)};
        found = true;
        break;
    }
    if (!found) {
        return false;
    }
    mechanics::ParticipantCreditEvent source{};
    const mechanics::ParticipantCreditResult result =
        slot.credits.record_contribution(mechanics_header(context, command),
                                         handle,
                                         static_cast<std::uint32_t>(value.channelId),
                                         value.amount,
                                         source);
    if (result != mechanics::ParticipantCreditResult::applied
        && result != mechanics::ParticipantCreditResult::saturated) {
        return false;
    }
    world::CommittedEvent event{};
    event.commandId = command.header.commandId;
    event.tick = context.tick;
    event.actorRevision = source.revision;
    event.value = source.participantId;
    event.kind = world::CommittedEventKind::participantCreditChanged;
    return writer.append(event);
}

/** Publishes one already allowlisted presentation intent. */
[[nodiscard]] bool emit_incident(const world::HostExecutionContext& context,
                                 const world::HostCommand& command,
                                 EventWriter& writer) noexcept {
    const auto& value = std::get<world::EmitMappedIncidentCommand>(command.payload);
    world::CommittedEvent event{};
    event.actor = value.sourceActor;
    event.commandId = command.header.commandId;
    event.tick = context.tick;
    event.value = value.incidentId;
    event.kind = world::CommittedEventKind::incidentSubmitted;
    return writer.append(event);
}

} // namespace

/** Applies one objective, credit, or allowlisted incident command. */
bool execute_ledger_command(BubbleHost::Storage::WorldSlot& slot,
                            const world::HostExecutionContext& context,
                            const world::HostCommand& command,
                            EventWriter& writer) noexcept {
    switch (world::command_kind(command)) {
    case world::HostCommandKind::addObjectiveCounter:
    case world::HostCommandKind::setObjectiveCounter:
        return apply_objective(slot, context, command, writer);
    case world::HostCommandKind::recordParticipantCredit:
        return apply_credit(slot, context, command, writer);
    case world::HostCommandKind::emitMappedIncident:
        return emit_incident(context, command, writer);
    default:
        return false;
    }
}

/** Schedules one durable logical timer before the runner remembers its command. */
bool execute_timer_command(BubbleHost::Storage::WorldSlot& slot,
                           const world::HostExecutionContext& context,
                           const world::HostCommand& command,
                           EventWriter& writer) noexcept {
    if (world::command_kind(command) != world::HostCommandKind::scheduleTickTimer) {
        return false;
    }
    const auto& schedule = std::get<world::ScheduleTickTimerCommand>(command.payload);
    mechanics::TickTimerHandle timer{};
    const mechanics::TickTimerResult result = slot.timers.schedule(
        mechanics_header(context, command), schedule.timerId, schedule.fireTick, timer);
    if (result != mechanics::TickTimerResult::applied
        && result != mechanics::TickTimerResult::duplicate) {
        return false;
    }
    world::CommittedEvent event{};
    event.commandId = command.header.commandId;
    event.tick = context.tick;
    event.actorRevision = timer.generation;
    event.value = schedule.timerId;
    event.kind = world::CommittedEventKind::serviceTickCommitted;
    return writer.append(event);
}

/** Preserves the explicit objective/credit stage after synchronous commands. */
bool run_objective_credit(BubbleHost::Storage::WorldSlot& slot,
                          const world::HostExecutionContext& context,
                          EventWriter& writer) noexcept {
    static_cast<void>(slot);
    static_cast<void>(context);
    static_cast<void>(writer);
    return true;
}

/** Fires due fixed-tick timers and copies immutable timer events. */
bool run_timers(BubbleHost::Storage::WorldSlot& slot,
                const world::HostExecutionContext& context,
                EventWriter& writer) noexcept {
    slot.timerEvents = {};
    slot.timerEventCount = 0;
    const mechanics::TickTimerResult result =
        slot.timers.fire_due(context.tick, slot.timerEvents, slot.timerEventCount);
    if (result != mechanics::TickTimerResult::applied
        && result != mechanics::TickTimerResult::noChange) {
        return false;
    }
    for (std::size_t index = 0; index < slot.timerEventCount; ++index) {
        const mechanics::TickTimerEvent& source = slot.timerEvents[index];
        world::CommittedEvent event{};
        event.commandId = 0;
        event.tick = context.tick;
        event.actorRevision = source.scheduleCommandId;
        event.value = source.timerId;
        event.kind = world::CommittedEventKind::timerFired;
        if (!writer.append(event)) {
            return false;
        }
    }
    return true;
}

} // namespace sunrise::server::gameplay::physics::host::detail
