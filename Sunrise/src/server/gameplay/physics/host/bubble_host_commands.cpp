#include "bubble_host.h"
#include "command_effects.h"

namespace sunrise::server::gameplay::physics::host {
namespace {

[[nodiscard]] world::HostCommandExecutionResult reject(world::CommandRejectReason reason) noexcept {
    world::HostCommandExecutionResult result{};
    result.rejectReason = reason;
    result.status = world::HostCommandExecutionStatus::rejected;
    return result;
}

/** @return Worst-case synchronous event count for one extension command. */
[[nodiscard]] std::size_t required_event_capacity(world::HostCommandKind kind) noexcept {
    switch (kind) {
    case world::HostCommandKind::submitDamage:
        return 3;
    case world::HostCommandKind::addObjectiveCounter:
    case world::HostCommandKind::setObjectiveCounter:
        return mechanics::kObjectiveThresholdCapacity + 1U;
    case world::HostCommandKind::requestPath:
    case world::HostCommandKind::configureCombat:
    case world::HostCommandKind::createTrigger:
    case world::HostCommandKind::removeTrigger:
    case world::HostCommandKind::recordParticipantCredit:
    case world::HostCommandKind::emitMappedIncident:
    case world::HostCommandKind::scheduleTickTimer:
        return 1;
    default:
        return 0;
    }
}

/** Opens one bounded service-command admission window. */
void begin_command_tick(BubbleHost::Storage::WorldSlot& slot, world::TickId tick) noexcept {
    if (slot.serviceCommandTick == tick) {
        return;
    }
    slot.timers.expire_commands_before(tick);
    slot.triggers.expire_commands_before(tick);
    slot.objectives.expire_commands_before(tick);
    slot.credits.expire_commands_before(tick);
    slot.combat.expire_commands_before(tick);
    slot.serviceCommandTick = tick;
    slot.queryCommandCount = 0;
    slot.timerCommandCount = 0;
    slot.triggerCommandCount = 0;
    slot.objectiveCommandCount = 0;
    slot.creditCommandCount = 0;
    slot.combatCommandCount = 0;
}

/** Applies one already validated command to its synchronous generic service. */
[[nodiscard]] bool apply_command(BubbleHost::Storage::WorldSlot& slot,
                                 const world::HostExecutionContext& context,
                                 const world::HostCommand& command,
                                 std::span<const world::ActorSnapshot> actors,
                                 detail::EventWriter& writer) noexcept {
    switch (world::command_kind(command)) {
    case world::HostCommandKind::configureController:
    case world::HostCommandKind::requestPath:
        return detail::execute_navigation_command(slot, context, command, actors, writer);
    case world::HostCommandKind::configureCombat:
        return detail::execute_combat_setup_command(slot, context, command, actors, writer);
    case world::HostCommandKind::submitDamage:
        return detail::execute_damage_command(slot, context, command, actors, writer);
    case world::HostCommandKind::createTrigger:
    case world::HostCommandKind::removeTrigger:
        return detail::execute_trigger_command(slot, context, command, writer);
    case world::HostCommandKind::addObjectiveCounter:
    case world::HostCommandKind::setObjectiveCounter:
    case world::HostCommandKind::recordParticipantCredit:
    case world::HostCommandKind::emitMappedIncident:
        return detail::execute_ledger_command(slot, context, command, writer);
    case world::HostCommandKind::scheduleTickTimer:
        return detail::execute_timer_command(slot, context, command, writer);
    default:
        return false;
    }
}

/** Accounts one applied command against its fixed per-tick service bound. */
void count_applied(BubbleHost::Storage::WorldSlot& slot, world::HostCommandKind kind) noexcept {
    switch (kind) {
    case world::HostCommandKind::requestPath:
        ++slot.queryCommandCount;
        break;
    case world::HostCommandKind::scheduleTickTimer:
        ++slot.timerCommandCount;
        break;
    case world::HostCommandKind::createTrigger:
    case world::HostCommandKind::removeTrigger:
        ++slot.triggerCommandCount;
        break;
    case world::HostCommandKind::addObjectiveCounter:
    case world::HostCommandKind::setObjectiveCounter:
        ++slot.objectiveCommandCount;
        break;
    case world::HostCommandKind::recordParticipantCredit:
        ++slot.creditCommandCount;
        break;
    case world::HostCommandKind::submitDamage:
        ++slot.combatCommandCount;
        break;
    default:
        break;
    }
}

} // namespace

/** Applies one non-core command before the runner remembers it. */
world::HostCommandExecutionResult
HostCommandProcessor::execute(BubbleHost& host,
                              const world::HostExecutionContext& context,
                              const world::HostCommand& command,
                              std::span<const world::ActorSnapshot> actors,
                              std::span<world::CommittedEvent> events) noexcept {
    const std::size_t slotIndex = host.find_world(context.world);
    if (slotIndex == kWorldCapacity || context.phase != world::HostExecutionPhase::commandCommit) {
        return reject(world::CommandRejectReason::wrongWorld);
    }
    BubbleHost::Storage::WorldSlot& slot = host.storage_->worlds[slotIndex];
    if (!slot.inTick || !slot.healthy) {
        return reject(world::CommandRejectReason::invalid);
    }
    begin_command_tick(slot, context.tick);
    const world::HostCommandKind kind = world::command_kind(command);
    if (events.size() < required_event_capacity(kind)) {
        return reject(world::CommandRejectReason::capacity);
    }
    const world::HostCommandExecutionResult validation =
        detail::validate_extension_command(slot, context, command, actors);
    if (validation.status != world::HostCommandExecutionStatus::applied) {
        return validation;
    }
    detail::EventWriter writer{events};
    if (!apply_command(slot, context, command, actors, writer)) {
        slot.healthy = false;
        world::HostCommandExecutionResult result{};
        result.status = world::HostCommandExecutionStatus::unhealthy;
        return result;
    }
    count_applied(slot, kind);
    world::HostCommandExecutionResult result{};
    result.eventCount = writer.count;
    result.rejectReason = world::CommandRejectReason::none;
    result.status = world::HostCommandExecutionStatus::applied;
    return result;
}

/** Routes the runner's generic command seam into fixed host services. */
world::HostCommandExecutionResult
BubbleHost::execute_command(const world::HostExecutionContext& context,
                            const world::HostCommand& command,
                            std::span<const world::ActorSnapshot> actors,
                            std::span<world::CommittedEvent> events) noexcept {
    return HostCommandProcessor::execute(*this, context, command, actors, events);
}

} // namespace sunrise::server::gameplay::physics::host
