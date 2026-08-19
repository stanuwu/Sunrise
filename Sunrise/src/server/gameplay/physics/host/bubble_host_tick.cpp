#include <algorithm>

#include "bubble_host.h"
#include "tick_runtime.h"

namespace sunrise::server::gameplay::physics::host {
namespace detail {

/** Appends one event without changing caller storage on overflow. */
bool EventWriter::append(const world::CommittedEvent& event) noexcept {
    if (count == events.size()) {
        return false;
    }
    events[count++] = event;
    return true;
}

/** Returns unused runner-owned event storage. */
std::span<world::CommittedEvent> EventWriter::remaining() const noexcept {
    return events.subspan(count);
}

} // namespace detail

/** Runs the intended generic-service order inside the runner's policy boundary. */
world::HostTickExecutionResult
HostTickProcessor::step(BubbleHost& host,
                        const world::HostExecutionContext& context,
                        std::span<const world::ActorSnapshot> actors,
                        std::span<const world::CommittedEvent> committedEvents,
                        std::span<world::CommittedEvent> events) noexcept {
    world::HostTickExecutionResult result{};
    const std::size_t slotIndex = host.find_world(context.world);
    if (slotIndex == kWorldCapacity || context.phase != world::HostExecutionPhase::serviceTick) {
        result.healthy = false;
        return result;
    }
    BubbleHost::Storage::WorldSlot& slot = host.storage_->worlds[slotIndex];
    std::array<world::CommittedEvent, world::kCommittedEventCapacity> stagedEvents{};
    detail::EventWriter writer{stagedEvents};
    bool healthy = slot.inTick && slot.healthy && context.triggerBudget == slot.triggerBudget
                   && context.queryBudget == slot.queryBudget
                   && context.counterBudget == slot.counterBudget
                   && context.creditBudget == slot.creditBudget;

    host.record_stage(slotIndex, TickStage::actorSync);
    healthy = healthy && detail::sync_actor_bodies(slot, context, actors, committedEvents);
    host.record_stage(slotIndex, TickStage::controllerNavigation);
    healthy = healthy
              && detail::run_controller_navigation(slot, context, actors, committedEvents, writer);
    host.record_stage(slotIndex, TickStage::physics);
    healthy = healthy && detail::run_physics(slot, context);
    healthy = healthy && detail::collect_pose_commits(slot, actors);
    host.record_stage(slotIndex, TickStage::combat);
    healthy = healthy && detail::run_combat(slot, context, actors, writer);
    host.record_stage(slotIndex, TickStage::triggers);
    healthy = healthy && detail::run_triggers(slot, context, writer);
    host.record_stage(slotIndex, TickStage::objectiveCredit);
    healthy = healthy && detail::run_objective_credit(slot, context, writer);
    host.record_stage(slotIndex, TickStage::timers);
    healthy = healthy && detail::run_timers(slot, context, writer);

    world::CommittedEvent serviceEvent{};
    serviceEvent.tick = context.tick;
    serviceEvent.value = slot.contacts.count;
    serviceEvent.kind = world::CommittedEventKind::serviceTickCommitted;
    healthy = healthy && writer.append(serviceEvent);
    const bool outputFits =
        writer.count <= events.size() && slot.poseCommitCount <= events.size() - writer.count;
    slot.healthy = slot.healthy && healthy && outputFits;
    if (slot.healthy) {
        std::copy_n(stagedEvents.begin(), writer.count, events.begin());
        result.eventCount = writer.count;
        result.poseCommits = std::span(slot.poseCommits).first(slot.poseCommitCount);
    }
    result.healthy = slot.healthy;
    return result;
}

/** Routes the runner's service phase through the composed generic host. */
world::HostTickExecutionResult
BubbleHost::step_services(const world::HostExecutionContext& context,
                          std::span<const world::ActorSnapshot> actors,
                          std::span<const world::CommittedEvent> committedEvents,
                          std::span<world::CommittedEvent> events) noexcept {
    return HostTickProcessor::step(*this, context, actors, committedEvents, events);
}

/** Runs one fixed tick and dispatches committed checkpoint requests last. */
TickResult BubbleHost::tick(WorldHandle handle) noexcept {
    TickResult result{};
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        result.status = storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
        return result;
    }
    Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    if (!slot.healthy || !slot.runner.healthy() || slot.inTick) {
        result.status = HostStatus::unhealthy;
        return result;
    }
    std::uint64_t journalFloor = 0;
    std::uint64_t journalTail = 0;
    const bool journalKnown =
        storage_->journal->cursors(slot.logicalWorldId, journalFloor, journalTail);
    static_cast<void>(journalFloor);
    if ((journalKnown && journalTail != slot.journalCursor)
        || (!journalKnown && slot.journalCursor != 0)) {
        result.status = HostStatus::unhealthy;
        return result;
    }
    if (storage_->journal->size()
        > persistence::kJournalRecordCapacity - world::kCommandQueueCapacity) {
        result.status = HostStatus::capacity;
        return result;
    }
    if (slot.runner.tick() == (std::numeric_limits<world::TickId>::max)()) {
        result.status = HostStatus::unhealthy;
        return result;
    }
    detail::capture_tick_backup(slot);
    if (!slot.tickBackup.valid) {
        result.status = HostStatus::unhealthy;
        return result;
    }
    const world::WorldIdentity identity = slot.runner.identity();
    const world::TickId nextTick = slot.runner.tick() + 1;
    if (slot.controller != nullptr && !slot.controller->begin_tick(identity, nextTick)) {
        slot.tickBackup.valid = false;
        result.status = HostStatus::serviceRejected;
        return result;
    }

    slot.tickStages = {};
    slot.tickStageCount = 0;
    slot.triggerEvents = {};
    slot.triggerEventCount = 0;
    slot.timerEvents = {};
    slot.timerEventCount = 0;
    slot.contacts = {};
    slot.poseCommits = {};
    slot.poseCommitCount = 0;
    slot.inTick = true;
    record_stage(slotIndex, TickStage::logicalCommit);
    result.world = slot.runner.advance(1);
    slot.inTick = false;
    if (result.world.ticksRun != 1 || !result.world.healthy) {
        if (slot.controller != nullptr) {
            slot.controller->rollback_tick(identity, nextTick);
        }
        const bool restored = detail::restore_tick_backup(slot);
        slot.healthy = restored && slot.healthy;
        result.status = HostStatus::unhealthy;
        return result;
    }
    journalTail = 0;
    const persistence::JournalResult journalResult = storage_->journal->append(
        slot.logicalWorldId, slot.journalCursor, slot.runner.committed_commands(), journalTail);
    if (journalResult != persistence::JournalResult::appended
        && journalResult != persistence::JournalResult::empty) {
        if (slot.controller != nullptr) {
            slot.controller->rollback_tick(identity, nextTick);
        }
        const bool restored = detail::restore_tick_backup(slot);
        slot.healthy = restored && slot.healthy;
        result.status = HostStatus::unhealthy;
        return result;
    }
    slot.journalCursor = journalTail;
    slot.tickBackup.valid = false;
    if (slot.controller != nullptr) {
        slot.controller->commit_tick(identity, nextTick);
    }
    record_stage(slotIndex, TickStage::policyPostTick);

    world::WorldSnapshot worldSnapshot{};
    if (!slot.runner.snapshot(worldSnapshot)) {
        slot.healthy = false;
    }
    for (const world::CommittedEvent& event : slot.runner.committed_events()) {
        if (event.kind != world::CommittedEventKind::checkpointRequested) {
            continue;
        }
        CheckpointRequest request{};
        request.identity = slot.runner.identity();
        request.world = handle;
        request.tick = event.tick;
        request.canonicalHash = worldSnapshot.canonicalHash;
        request.reasonId = event.value;
        request.logicalWorldId = slot.logicalWorldId;
        request.journalCursor = slot.journalCursor;
        ++result.checkpointCount;
        const bool accepted =
            slot.checkpointSink != nullptr && slot.checkpointSink->request_checkpoint(request);
        if (accepted) {
            continue;
        }
        if (slot.checkpointRequestCount == slot.checkpointRequests.size()) {
            slot.healthy = false;
            continue;
        }
        slot.checkpointRequests[slot.checkpointRequestCount++] = request;
    }
    record_stage(slotIndex, TickStage::checkpointDispatch);

    result.stageCount = slot.tickStageCount;
    for (std::size_t index = 0; index < result.stageCount; ++index) {
        result.stages[index] = slot.tickStages[index];
    }
    result.contactCount = slot.contacts.count;
    result.triggerEventCount = slot.triggerEventCount;
    result.timerEventCount = slot.timerEventCount;
    result.status =
        slot.healthy && slot.runner.healthy() ? HostStatus::success : HostStatus::unhealthy;
    return result;
}

} // namespace sunrise::server::gameplay::physics::host
