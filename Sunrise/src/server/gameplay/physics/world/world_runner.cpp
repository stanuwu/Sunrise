#include "world_runner.h"

#include <algorithm>
#include <limits>

namespace sunrise::server::gameplay::physics::world {
namespace {

/** @return True when every fixed policy limit fits the runner. */
[[nodiscard]] bool valid_manifest(const ActivityPolicyManifest& manifest) noexcept {
    return valid_blueprint(manifest.policy) && manifest.fixedRateHz != 0
           && manifest.fixedRateHz <= kMaximumFixedRateHz && manifest.actorBudget <= kActorCapacity
           && (manifest.allowedCommandMask >> static_cast<std::uint8_t>(HostCommandKind::count))
                  == 0;
}

} // namespace

/**
 * Opens one fresh world generation and initializes its selected policy.
 * @param config Stable activity, owner, and deterministic seed values.
 * @param policy Optional mission policy. Null selects the empty fallback.
 * @return True when the world and policy initialized together.
 */
bool WorldRunner::open(const WorldOpenConfig& config, IActivityPolicy* policy) noexcept {
    if (open_ || config.activitySessionId == 0 || config.ownerGeneration == 0
        || nextWorldGeneration_ == 0) {
        return false;
    }

    IActivityPolicy* selectedPolicy = policy == nullptr ? &defaultPolicy_ : policy;
    const ActivityPolicyManifest selectedManifest = selectedPolicy->manifest();
    if (!valid_manifest(selectedManifest)) {
        return false;
    }

    identity_.activitySessionId = config.activitySessionId;
    identity_.worldGeneration = nextWorldGeneration_++;
    identity_.ownerGeneration = config.ownerGeneration;
    manifest_ = selectedManifest;
    policyContext_.world = identity_;
    policyContext_.deterministicSeed = config.deterministicSeed;
    policyContext_.fixedRateHz = manifest_.fixedRateHz;
    policy_ = selectedPolicy;
    executor_ = config.executor;
    actors_.reset();
    commandQueue_.reset();
    tickCommands_ = {};
    committedCommands_ = {};
    tickCommandApplied_ = {};
    tickCommandExtension_ = {};
    committedEvents_ = {};
    commandHistory_ = {};
    executorActors_ = {};
    metrics_ = {};
    tick_ = 0;
    policySourceSequence_ = 0;
    committedEventCount_ = 0;
    committedCommandCount_ = 0;
    commandHistoryCount_ = 0;
    commandHistoryCursor_ = 0;
    tickDebt_ = 0;
    open_ = true;
    healthy_ = true;
    allowHostActorCommands_ = config.allowHostActorCommands;
    queueOverflowObserved_ = false;
    tickTransactionActive_ = false;

    CommandSource source{};
    source.id = manifest_.policy.id;
    source.kind = CommandSourceKind::policy;
    HostCommands commands(commandQueue_, manifest_, identity_, source, 1, policySourceSequence_);
    if (!policy_->initialize(policyContext_, commands)) {
        close();
        return false;
    }
    canonicalHash_ = compute_hash();
    return true;
}

/** Clears one world while preserving the process-local generation source. */
void WorldRunner::close() noexcept {
    commandQueue_.reset();
    actors_.reset();
    tickCommands_ = {};
    committedCommands_ = {};
    tickCommandApplied_ = {};
    tickCommandExtension_ = {};
    committedEvents_ = {};
    commandHistory_ = {};
    metrics_ = {};
    manifest_ = {};
    policyContext_ = {};
    identity_ = {};
    policy_ = nullptr;
    executor_ = nullptr;
    tick_ = 0;
    policySourceSequence_ = 0;
    canonicalHash_ = 0;
    committedEventCount_ = 0;
    committedCommandCount_ = 0;
    commandHistoryCount_ = 0;
    commandHistoryCursor_ = 0;
    tickDebt_ = 0;
    open_ = false;
    healthy_ = false;
    allowHostActorCommands_ = false;
    queueOverflowObserved_ = false;
    tickTransactionActive_ = false;
}

/**
 * Admits one exact future command to the bounded ingress queue.
 * @param command Typed command with complete identity and source order.
 * @return Admission status without changing world state.
 */
CommandSubmitStatus WorldRunner::submit(const HostCommand& command) noexcept {
    if (!open_ || !valid_command_header(command.header)) {
        return CommandSubmitStatus::invalid;
    }
    if (!equal_world(command.header.world, identity_)) {
        return CommandSubmitStatus::wrongWorld;
    }
    const HostCommandKind kind = command_kind(command);
    if ((command.header.source.kind == CommandSourceKind::network
         && kind != HostCommandKind::setKinematicTarget)
        || command.header.source.kind == CommandSourceKind::timer) {
        return CommandSubmitStatus::policyDenied;
    }
    if (command.header.source.kind == CommandSourceKind::host && !allowHostActorCommands_
        && command_uses_actor(kind)) {
        return CommandSubmitStatus::policyDenied;
    }
    if (command.header.executeTick <= tick_) {
        return CommandSubmitStatus::late;
    }
    if (command.header.source.kind == CommandSourceKind::policy
        && (command.header.source.id != manifest_.policy.id
            || (manifest_.allowedCommandMask & command_kind_mask(command_kind(command))) == 0)) {
        return CommandSubmitStatus::policyDenied;
    }
    return commandQueue_.push(command);
}

/**
 * Runs at most the fixed catch-up limit without changing fixed delta.
 * @param dueTicks Scheduler ticks due since the previous call.
 * @return Work completed, deferred debt, and health state.
 */
AdvanceResult WorldRunner::advance(std::uint32_t dueTicks) noexcept {
    AdvanceResult result{};
    result.lastTick = tick_;
    result.healthy = healthy_;
    if (!open_ || !healthy_) {
        return result;
    }

    if (commandQueue_.overflowed() && !queueOverflowObserved_) {
        queueOverflowObserved_ = true;
        ++metrics_.queueOverflows;
        healthy_ = false;
        result.healthy = false;
        return result;
    }

    const std::uint32_t room = std::numeric_limits<std::uint32_t>::max() - tickDebt_;
    tickDebt_ += std::min(room, dueTicks);
    const bool excessiveDebt = tickDebt_ > kMaximumTickDebt;
    const std::uint32_t runCount = std::min(tickDebt_, kMaximumCatchUpTicks);
    for (std::uint32_t index = 0; index < runCount; ++index) {
        if (!run_tick()) {
            healthy_ = false;
            break;
        }
        --tickDebt_;
        ++result.ticksRun;
    }
    if (excessiveDebt) {
        healthy_ = false;
    }
    result.lastTick = tick_;
    result.ticksDeferred = tickDebt_;
    result.healthy = healthy_;
    return result;
}

/**
 * Copies one pointer-free committed snapshot.
 * @param output Fixed caller-owned snapshot storage.
 * @return True when a world is open.
 */
bool WorldRunner::snapshot(WorldSnapshot& output) const noexcept {
    output = {};
    if (!open_) {
        return false;
    }
    output.identity = identity_;
    output.tick = tick_;
    output.actorCount = actors_.copy_sorted(output.actors);
    output.retiredActorCount = actors_.copy_retired(output.retiredActors);
    output.commandHistory = commandHistory_;
    output.commandHistoryCount = commandHistoryCount_;
    output.commandHistoryCursor = commandHistoryCursor_;
    output.canonicalHash = canonicalHash_;
    output.healthy = healthy_;
    return true;
}

bool WorldRunner::is_open() const noexcept {
    return open_;
}

bool WorldRunner::healthy() const noexcept {
    return healthy_;
}

WorldIdentity WorldRunner::identity() const noexcept {
    return identity_;
}

TickId WorldRunner::tick() const noexcept {
    return tick_;
}

std::uint32_t WorldRunner::fixed_rate_hz() const noexcept {
    return manifest_.fixedRateHz;
}

std::size_t WorldRunner::actor_count() const noexcept {
    return actors_.size();
}

std::size_t WorldRunner::queued_command_count() const noexcept {
    return commandQueue_.size();
}

std::span<const CommittedEvent> WorldRunner::committed_events() const noexcept {
    return std::span(committedEvents_).first(committedEventCount_);
}

std::span<const HostCommand> WorldRunner::committed_commands() const noexcept {
    return std::span(committedCommands_).first(committedCommandCount_);
}

const WorldMetrics& WorldRunner::metrics() const noexcept {
    return metrics_;
}

/** @return True when one fixed tick and both policy callbacks complete. */
bool WorldRunner::run_tick() noexcept {
    if (!open_ || policy_ == nullptr || tick_ == std::numeric_limits<TickId>::max()) {
        return false;
    }
    if (!begin_tick_transaction()) {
        healthy_ = false;
        return false;
    }

    ++tick_;
    committedEventCount_ = 0;
    committedCommandCount_ = 0;
    committedCommands_ = {};
    tickCommandApplied_ = {};
    tickCommandExtension_ = {};
    const std::size_t commandCount = commandQueue_.drain_tick(tick_, tickCommands_);
    for (std::size_t index = 0; index < commandCount; ++index) {
        apply_command(tickCommands_[index], index);
        if (!healthy_) {
            break;
        }
    }
    if (!healthy_) {
        rollback_tick_transaction();
        return false;
    }

    PolicyTickContext tickContext{};
    tickContext.tick = tick_;
    tickContext.fixedDeltaDenominator = manifest_.fixedRateHz;
    CommandSource source{};
    source.id = manifest_.policy.id;
    source.kind = CommandSourceKind::policy;
    const TickId nextTick = tick_ == std::numeric_limits<TickId>::max() ? tick_ : tick_ + 1U;
    HostCommands commands(
        commandQueue_, manifest_, identity_, source, nextTick, policySourceSequence_);
    policy_->pre_tick(tickContext, commands);
    if (commandQueue_.overflowed()) {
        rollback_tick_transaction();
        return false;
    }
    if (!step_extension_services(commandCount)) {
        rollback_tick_transaction();
        return false;
    }
    finalize_committed_commands(commandCount, true);
    policy_->post_tick(tickContext, committed_events(), commands);
    if (commandQueue_.overflowed()) {
        rollback_tick_transaction();
        return false;
    }
    canonicalHash_ = compute_hash();
    ++metrics_.ticksRun;
    commit_tick_transaction();
    return true;
}

} // namespace sunrise::server::gameplay::physics::world
