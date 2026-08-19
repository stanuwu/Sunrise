#include "command_queue.h"

#include <algorithm>
#include <limits>

namespace sunrise::server::gameplay::physics::world {
namespace {

/** Holds one SRW lock exclusively until the scope ends. */
class ExclusiveLock final {
public:
    explicit ExclusiveLock(SRWLOCK& lock) noexcept : lock_(&lock) {
        AcquireSRWLockExclusive(lock_);
    }
    ~ExclusiveLock() {
        ReleaseSRWLockExclusive(lock_);
    }
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock(ExclusiveLock&&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(ExclusiveLock&&) = delete;

private:
    SRWLOCK* lock_{};
};

[[nodiscard]] bool same_source(const CommandSource& left, const CommandSource& right) noexcept {
    return left.kind == right.kind && left.id == right.id;
}

[[nodiscard]] bool same_command(const HostCommand& left, const HostCommand& right) noexcept {
    return same_source(left.header.source, right.header.source)
           && left.header.commandId == right.header.commandId;
}

/**
 * Checks whether a newer valid sample can replace one queued motion command.
 * @param queued Existing queue entry.
 * @param incoming Candidate replacement.
 * @return True when every identity and authority boundary matches.
 */
[[nodiscard]] bool can_coalesce_motion(const HostCommand& queued,
                                       const HostCommand& incoming) noexcept {
    const auto* queuedMotion = std::get_if<SetKinematicTargetCommand>(&queued.payload);
    const auto* incomingMotion = std::get_if<SetKinematicTargetCommand>(&incoming.payload);
    return queuedMotion != nullptr && incomingMotion != nullptr && valid_actor(queuedMotion->actor)
           && valid_actor(incomingMotion->actor) && queuedMotion->authorityGeneration != 0
           && incomingMotion->authorityGeneration != 0 && finite_transform(queuedMotion->target)
           && finite_transform(incomingMotion->target)
           && same_source(queued.header.source, incoming.header.source)
           && queued.header.executeTick == incoming.header.executeTick
           && queued.header.policyOwnerId == incoming.header.policyOwnerId
           && queued.header.definition.id == incoming.header.definition.id
           && queued.header.definition.version == incoming.header.definition.version
           && incoming.header.sourceSequence > queued.header.sourceSequence
           && equal_actor(queuedMotion->actor, incomingMotion->actor)
           && queuedMotion->authorityGeneration == incomingMotion->authorityGeneration;
}

} // namespace

/** Clears all ingress and overflow state under the queue lock. */
void CommandQueue::reset() noexcept {
    const ExclusiveLock lock(mutex_);
    entries_ = {};
    rollbackEntries_ = {};
    nextIngressSerial_ = 1;
    rollbackNextIngressSerial_ = 1;
    size_ = 0;
    rollbackSize_ = 0;
    overflowed_ = false;
    rollbackOverflowed_ = false;
    transactionActive_ = false;
}

/** Saves exact pending ingress before one writer tick starts. */
bool CommandQueue::begin_transaction() noexcept {
    const ExclusiveLock lock(mutex_);
    if (transactionActive_) {
        return false;
    }
    rollbackEntries_ = entries_;
    rollbackNextIngressSerial_ = nextIngressSerial_;
    rollbackSize_ = size_;
    rollbackOverflowed_ = overflowed_;
    transactionActive_ = true;
    return true;
}

/** Keeps writer and policy ingress accepted by a completed tick. */
void CommandQueue::commit_transaction() noexcept {
    const ExclusiveLock lock(mutex_);
    transactionActive_ = false;
}

/** Restores exact ingress from before a failed writer tick. */
void CommandQueue::rollback_transaction() noexcept {
    const ExclusiveLock lock(mutex_);
    if (!transactionActive_) {
        return;
    }
    entries_ = rollbackEntries_;
    nextIngressSerial_ = rollbackNextIngressSerial_;
    size_ = rollbackSize_;
    overflowed_ = rollbackOverflowed_;
    transactionActive_ = false;
}

/**
 * Adds or coalesces one typed command under the queue lock.
 * @param command Fully stamped future command.
 * @return Admission status without world mutation.
 */
CommandSubmitStatus CommandQueue::push(const HostCommand& command) noexcept {
    if (!valid_command_header(command.header) || command_kind(command) == HostCommandKind::count) {
        return CommandSubmitStatus::invalid;
    }

    const ExclusiveLock lock(mutex_);
    if (nextIngressSerial_ == 0
        || nextIngressSerial_ == std::numeric_limits<std::uint64_t>::max()) {
        overflowed_ = true;
        return CommandSubmitStatus::full;
    }
    for (Entry& entry : entries_) {
        if (!entry.occupied) {
            continue;
        }
        if (same_command(entry.command, command)) {
            return CommandSubmitStatus::duplicate;
        }
        if (same_source(entry.command.header.source, command.header.source)
            && entry.command.header.sourceSequence == command.header.sourceSequence) {
            return CommandSubmitStatus::invalid;
        }
        if (can_coalesce_motion(entry.command, command)) {
            entry.command = command;
            entry.ingressSerial = nextIngressSerial_++;
            return CommandSubmitStatus::coalesced;
        }
    }

    if (size_ == entries_.size()) {
        overflowed_ = true;
        return CommandSubmitStatus::full;
    }

    for (Entry& entry : entries_) {
        if (!entry.occupied) {
            entry.command = command;
            entry.ingressSerial = nextIngressSerial_++;
            entry.occupied = true;
            ++size_;
            return CommandSubmitStatus::accepted;
        }
    }

    overflowed_ = true;
    return CommandSubmitStatus::full;
}

/**
 * Sorts and removes commands for one exact tick under the queue lock.
 * @param tick Exact execution tick selected by the writer.
 * @param output Fixed writer-owned destination.
 * @return Number of ordered commands written.
 */
std::size_t CommandQueue::drain_tick(TickId tick, std::span<HostCommand> output) noexcept {
    const ExclusiveLock lock(mutex_);
    std::sort(
        entries_.begin(), entries_.end(), [](const Entry& first, const Entry& second) noexcept {
            if (first.occupied != second.occupied) {
                return first.occupied;
            }
            if (!first.occupied) {
                return false;
            }
            if (first.command.header.executeTick != second.command.header.executeTick) {
                return first.command.header.executeTick < second.command.header.executeTick;
            }
            if (first.command.header.source.kind != second.command.header.source.kind) {
                return first.command.header.source.kind < second.command.header.source.kind;
            }
            if (first.command.header.source.id != second.command.header.source.id) {
                return first.command.header.source.id < second.command.header.source.id;
            }
            if (first.command.header.sourceSequence != second.command.header.sourceSequence) {
                return first.command.header.sourceSequence < second.command.header.sourceSequence;
            }
            return first.ingressSerial < second.ingressSerial;
        });

    std::size_t written = 0;
    for (Entry& entry : entries_) {
        if (!entry.occupied || entry.command.header.executeTick != tick) {
            continue;
        }
        if (written == output.size()) {
            overflowed_ = true;
            break;
        }
        output[written++] = entry.command;
        entry = {};
        --size_;
    }

    std::size_t destination = 0;
    for (std::size_t source = 0; source < entries_.size(); ++source) {
        if (!entries_[source].occupied) {
            continue;
        }
        if (destination != source) {
            entries_[destination] = entries_[source];
            entries_[source] = {};
        }
        ++destination;
    }
    return written;
}

/** @return Current fixed queue use under the queue lock. */
std::size_t CommandQueue::size() const noexcept {
    const ExclusiveLock lock(mutex_);
    return size_;
}

/** @return True when a critical command could not enter fixed storage. */
bool CommandQueue::overflowed() const noexcept {
    const ExclusiveLock lock(mutex_);
    return overflowed_;
}

} // namespace sunrise::server::gameplay::physics::world
