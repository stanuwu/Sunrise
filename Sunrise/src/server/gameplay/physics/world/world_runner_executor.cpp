#include <algorithm>

#include "world_runner.h"

namespace sunrise::server::gameplay::physics::world {
namespace {

/** @return True when an executor rejection can enter bounded metrics. */
[[nodiscard]] bool valid_reject_reason(CommandRejectReason reason) noexcept {
    return reason != CommandRejectReason::none
           && static_cast<std::uint8_t>(reason)
                  < static_cast<std::uint8_t>(CommandRejectReason::count);
}

/**
 * Checks copied executor events before the runner publishes them.
 * @param events Candidate committed event range.
 * @param tick Exact current writer tick.
 * @param commandId Required command id, or zero for a service step.
 * @return True when every event belongs to this boundary.
 */
[[nodiscard]] bool valid_executor_events(std::span<const CommittedEvent> events,
                                         TickId tick,
                                         CommandId commandId) noexcept {
    for (const CommittedEvent& event : events) {
        if (event.tick != tick
            || static_cast<std::uint8_t>(event.kind)
                   >= static_cast<std::uint8_t>(CommittedEventKind::count)
            || event.commandId != commandId) {
            return false;
        }
    }
    return true;
}

} // namespace

/**
 * Routes one non-core typed command through the optional generic-service seam.
 * @param command Ordered typed command.
 * @param commandOrdinal Stable position in this tick's command batch.
 * @return True when the executor committed the command and copied valid events.
 */
bool WorldRunner::execute_extension_command(const HostCommand& command,
                                            std::size_t commandOrdinal) noexcept {
    if (executor_ == nullptr) {
        reject(CommandRejectReason::invalid);
        return false;
    }

    const std::size_t actorCount = actors_.copy_sorted(executorActors_);
    HostExecutionContext context{};
    context.world = identity_;
    context.tick = tick_;
    context.commandOrdinal = commandOrdinal;
    context.fixedDeltaDenominator = manifest_.fixedRateHz;
    context.triggerBudget = manifest_.triggerBudget;
    context.queryBudget = manifest_.queryBudget;
    context.counterBudget = manifest_.counterBudget;
    context.creditBudget = manifest_.creditBudget;
    context.phase = HostExecutionPhase::commandCommit;
    const std::size_t available = committedEvents_.size() - committedEventCount_;
    std::span<CommittedEvent> output =
        std::span(committedEvents_).subspan(committedEventCount_, available);
    const HostCommandExecutionResult result = executor_->execute_command(
        context, command, std::span(executorActors_).first(actorCount), output);

    if (result.status == HostCommandExecutionStatus::rejected) {
        if (result.eventCount != 0 || !valid_reject_reason(result.rejectReason)) {
            healthy_ = false;
            reject(CommandRejectReason::invalid);
            return false;
        }
        reject(result.rejectReason);
        return false;
    }
    if (result.status != HostCommandExecutionStatus::applied || result.eventCount > available) {
        healthy_ = false;
        reject(result.eventCount > available ? CommandRejectReason::capacity
                                             : CommandRejectReason::invalid);
        return false;
    }

    const std::span<const CommittedEvent> candidate = output.first(result.eventCount);
    if (!valid_executor_events(candidate, tick_, command.header.commandId)) {
        healthy_ = false;
        reject(CommandRejectReason::invalid);
        return false;
    }
    committedEventCount_ += result.eventCount;
    return true;
}

/**
 * Steps optional generic services between policy pre-tick and post-tick.
 * @param commandCount Number of ordered commands admitted this tick.
 * @return True when the service step stayed healthy and copied valid events.
 */
bool WorldRunner::step_extension_services(std::size_t commandCount) noexcept {
    if (executor_ == nullptr) {
        return true;
    }

    const std::size_t actorCount = actors_.copy_sorted(executorActors_);
    HostExecutionContext context{};
    context.world = identity_;
    context.tick = tick_;
    context.commandOrdinal = commandCount;
    context.fixedDeltaDenominator = manifest_.fixedRateHz;
    context.triggerBudget = manifest_.triggerBudget;
    context.queryBudget = manifest_.queryBudget;
    context.counterBudget = manifest_.counterBudget;
    context.creditBudget = manifest_.creditBudget;
    context.phase = HostExecutionPhase::serviceTick;
    const std::size_t eventStart = committedEventCount_;
    const std::size_t available = committedEvents_.size() - eventStart;
    const std::span<const CommittedEvent> input =
        std::span<const CommittedEvent>(committedEvents_).first(eventStart);
    std::span<CommittedEvent> output = std::span(committedEvents_).subspan(eventStart, available);
    const HostTickExecutionResult result = executor_->step_services(
        context, std::span(executorActors_).first(actorCount), input, output);
    const bool poseCapacity = result.poseCommits.size() <= available
                              && result.eventCount <= available - result.poseCommits.size();
    if (!result.healthy || result.eventCount > available || !poseCapacity
        || !valid_executor_events(output.first(std::min(result.eventCount, available)), tick_, 0)) {
        healthy_ = false;
        return false;
    }
    if (!commit_service_poses(result.poseCommits, output.subspan(result.eventCount))) {
        healthy_ = false;
        return false;
    }
    committedEventCount_ += result.eventCount + result.poseCommits.size();
    return true;
}

/** Validates and commits one complete commandless service pose batch. */
bool WorldRunner::commit_service_poses(std::span<const CanonicalPoseCommit> commits,
                                       std::span<CommittedEvent> events) noexcept {
    if (commits.size() > events.size() || commits.size() > executorActors_.size()) {
        return false;
    }
    for (const CanonicalPoseCommit& commit : commits) {
        if (authority_.validate_pose_commit(actors_, commit, tick_) != AuthorityStatus::applied) {
            return false;
        }
    }
    if (!actors_.commit_pose_batch(commits, executorActors_)) {
        return false;
    }
    for (std::size_t index = 0; index < commits.size(); ++index) {
        const ActorSnapshot& actor = executorActors_[index];
        events[index] = {};
        events[index].actor = actor.key;
        events[index].transform = actor.transform;
        events[index].tick = tick_;
        events[index].actorRevision = actor.revision;
        events[index].authorityGeneration = actor.authority.generation;
        events[index].kind = CommittedEventKind::transformChanged;
    }
    return true;
}

} // namespace sunrise::server::gameplay::physics::world
