#include <algorithm>
#include <limits>

#include "world_runner.h"

namespace sunrise::server::gameplay::physics::world {
namespace {

[[nodiscard]] bool same_command(const HostCommandHeader& left,
                                const HostCommandHeader& right) noexcept {
    return left.source.kind == right.source.kind && left.source.id == right.source.id
           && left.commandId == right.commandId;
}

/** @return True when retained command identities are exact and unique. */
[[nodiscard]] bool valid_history(const WorldSnapshot& snapshot) noexcept {
    if (snapshot.commandHistoryCount > snapshot.commandHistory.size()
        || snapshot.commandHistoryCursor != 0) {
        return false;
    }
    for (std::size_t index = 0; index < snapshot.commandHistoryCount; ++index) {
        const HostCommandHeader& header = snapshot.commandHistory[index];
        if (!valid_command_header(header) || !equal_world(header.world, snapshot.identity)
            || header.executeTick > snapshot.tick
            || (index != 0
                && snapshot.commandHistory[index - 1].executeTick > header.executeTick)) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (same_command(snapshot.commandHistory[prior], header)) {
                return false;
            }
        }
    }
    return true;
}

/** @return True when authority activation fits the restored writer tick. */
[[nodiscard]] bool valid_actor_ticks(const WorldSnapshot& snapshot) noexcept {
    const TickId maximumValidFrom =
        snapshot.tick == std::numeric_limits<TickId>::max() ? snapshot.tick : snapshot.tick + 1U;
    for (std::size_t index = 0; index < snapshot.actorCount; ++index) {
        const TickId validFrom = snapshot.actors[index].authority.validFromTick;
        if (validFrom == 0 || validFrom > maximumValidFrom) {
            return false;
        }
    }
    for (std::size_t index = 0; index < snapshot.retiredActorCount; ++index) {
        if (snapshot.retiredActors[index].retiredTick > snapshot.tick) {
            return false;
        }
    }
    return true;
}

/** @return Highest retained policy source order after recovery. */
[[nodiscard]] std::uint64_t policy_sequence(const WorldSnapshot& snapshot,
                                            BlueprintRef policy) noexcept {
    std::uint64_t sequence = 0;
    for (std::size_t index = 0; index < snapshot.commandHistoryCount; ++index) {
        const HostCommandHeader& header = snapshot.commandHistory[index];
        if (header.source.kind == CommandSourceKind::policy && header.source.id == policy.id) {
            sequence = std::max(sequence, header.sourceSequence);
        }
    }
    return sequence;
}

} // namespace

/** Restores logical state only on a fresh runner with exact rebound identity. */
bool WorldRunner::restore(const WorldSnapshot& snapshot) noexcept {
    if (!open_ || !healthy_ || tick_ != 0 || snapshot.actorCount > snapshot.actors.size()
        || snapshot.retiredActorCount > snapshot.retiredActors.size() || !snapshot.healthy
        || !equal_world(snapshot.identity, identity_) || !valid_history(snapshot)
        || !valid_actor_ticks(snapshot)) {
        return false;
    }
    const std::uint64_t expectedHash = compute_snapshot_hash(snapshot);
    if ((snapshot.canonicalHash != 0 && snapshot.canonicalHash != expectedHash)
        || !actors_.restore(std::span(snapshot.actors).first(snapshot.actorCount),
                            std::span(snapshot.retiredActors).first(snapshot.retiredActorCount))) {
        return false;
    }

    commandQueue_.reset();
    tickCommands_ = {};
    committedCommands_ = {};
    tickCommandApplied_ = {};
    tickCommandExtension_ = {};
    committedEvents_ = {};
    executorActors_ = {};
    commandHistory_ = {};
    std::copy_n(
        snapshot.commandHistory.begin(), snapshot.commandHistoryCount, commandHistory_.begin());
    metrics_ = {};
    tick_ = snapshot.tick;
    policySourceSequence_ = policy_sequence(snapshot, manifest_.policy);
    committedEventCount_ = 0;
    committedCommandCount_ = 0;
    commandHistoryCount_ = snapshot.commandHistoryCount;
    commandHistoryCursor_ = 0;
    tickDebt_ = 0;
    queueOverflowObserved_ = false;
    canonicalHash_ = compute_hash();
    return true;
}

} // namespace sunrise::server::gameplay::physics::world
