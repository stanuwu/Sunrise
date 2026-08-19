#include <limits>

#include "world_runner.h"

namespace sunrise::server::gameplay::physics::world {

/**
 * Appends one immutable event after its state change commits.
 * @param command Source command identity.
 * @param kind Committed event kind.
 * @param actor Pointer-free committed actor state.
 * @param value Event-specific scalar value.
 */
void WorldRunner::publish_event(const HostCommand& command,
                                CommittedEventKind kind,
                                const ActorSnapshot& actor,
                                std::uint64_t value) noexcept {
    if (committedEventCount_ == committedEvents_.size()) {
        healthy_ = false;
        return;
    }
    CommittedEvent& event = committedEvents_[committedEventCount_++];
    event.actor = actor.key;
    event.transform = actor.transform;
    event.commandId = command.header.commandId;
    event.tick = tick_;
    event.actorRevision = actor.revision;
    event.authorityGeneration = actor.authority.generation;
    event.value = value;
    event.kind = kind;
}

/** @param reason Bounded reason counter to increment. */
void WorldRunner::reject(CommandRejectReason reason) noexcept {
    ++metrics_.commandsRejected;
    const std::size_t index = static_cast<std::size_t>(reason);
    if (index < metrics_.rejectedByReason.size()) {
        ++metrics_.rejectedByReason[index];
    }
}

/**
 * Checks retained command identities above the durable replay floor.
 * @param header Source and deterministic command id.
 * @return True when the window already contains the command.
 */
bool WorldRunner::seen_command(const HostCommandHeader& header) const noexcept {
    for (std::size_t index = 0; index < commandHistoryCount_; ++index) {
        const HostCommandHeader& seen = commandHistory_[index];
        if (seen.source.kind == header.source.kind && seen.source.id == header.source.id
            && seen.commandId == header.commandId) {
            return true;
        }
    }
    return false;
}

/** @param header Applied command identity retained until explicit expiry. */
void WorldRunner::remember_command(const HostCommandHeader& header) noexcept {
    if (commandHistoryCount_ == commandHistory_.size()) {
        healthy_ = false;
    } else {
        commandHistory_[commandHistoryCount_++] = header;
    }
}

/** Copies applied commands after the service boundary decides extension fate. */
void WorldRunner::finalize_committed_commands(std::size_t commandCount,
                                              bool includeExtensions) noexcept {
    committedCommands_ = {};
    committedCommandCount_ = 0;
    for (std::size_t index = 0; index < commandCount; ++index) {
        if (tickCommandApplied_[index] && (includeExtensions || !tickCommandExtension_[index])) {
            committedCommands_[committedCommandCount_++] = tickCommands_[index];
        }
        tickCommands_[index] = {};
    }
    tickCommandApplied_ = {};
    tickCommandExtension_ = {};
}

/** Releases only identities below a caller-confirmed durable replay floor. */
bool WorldRunner::expire_commands_before(TickId firstRetainedTick) noexcept {
    const bool onePastTick =
        tick_ != std::numeric_limits<TickId>::max() && firstRetainedTick == tick_ + 1U;
    if (!open_ || firstRetainedTick == 0 || (firstRetainedTick > tick_ && !onePastTick)) {
        return false;
    }
    std::size_t retained = 0;
    for (std::size_t index = 0; index < commandHistoryCount_; ++index) {
        if (commandHistory_[index].executeTick >= firstRetainedTick) {
            commandHistory_[retained++] = commandHistory_[index];
        }
    }
    for (std::size_t index = retained; index < commandHistoryCount_; ++index) {
        commandHistory_[index] = {};
    }
    commandHistoryCount_ = retained;
    commandHistoryCursor_ = 0;
    actors_.expire_generations_before(firstRetainedTick);
    canonicalHash_ = compute_hash();
    return true;
}

} // namespace sunrise::server::gameplay::physics::world
