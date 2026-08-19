#include "checkpoint_hash_internal.h"

namespace sunrise::server::gameplay::physics::persistence::detail {

/** Appends opaque policy bytes without reading unused capacity. */
void CheckpointHashWriter::bytes(std::span<const std::byte> values) noexcept {
    for (const std::byte value : values) {
        hash_ ^= std::to_integer<std::uint8_t>(value);
        hash_ *= 1'099'511'628'211ULL;
    }
}

/** Returns the current immutable canonical hash. */
std::uint64_t CheckpointHashWriter::finish() const noexcept {
    return hash_;
}

} // namespace sunrise::server::gameplay::physics::persistence::detail

namespace sunrise::server::gameplay::physics::persistence {

/** Computes a complete logical checkpoint hash with no padding or pointer bytes. */
std::uint64_t compute_checkpoint_hash(const Checkpoint& checkpoint) noexcept {
    if (checkpoint.policyState.size > checkpoint.policyState.bytes.size()
        || checkpoint.world.actorCount > checkpoint.world.actors.size()
        || checkpoint.world.retiredActorCount > checkpoint.world.retiredActors.size()
        || checkpoint.world.commandHistoryCount > checkpoint.world.commandHistory.size()) {
        return 0;
    }
    detail::CheckpointHashWriter hash{};
    hash.scalar(checkpoint.checkpointId);
    hash.scalar(checkpoint.logicalWorldId);
    hash.scalar(checkpoint.contentBuildId);
    hash.scalar(checkpoint.deterministicRngState);
    hash.scalar(checkpoint.journalCursor);
    hash.scalar(checkpoint.schemaVersion);
    hash.scalar(checkpoint.fixedRateHz);
    hash.scalar(checkpoint.policy.id);
    hash.scalar(checkpoint.policy.version);
    hash.scalar(checkpoint.world.canonicalHash);
    hash.scalar(checkpoint.mechanics.worldEpoch);
    hash.scalar(checkpoint.policyState.size);
    hash.bytes(std::span(checkpoint.policyState.bytes).first(checkpoint.policyState.size));
    detail::hash_timers(hash, checkpoint.mechanics.timers);
    detail::hash_triggers(hash, checkpoint.mechanics.triggers);
    detail::hash_objectives(hash, checkpoint.mechanics.objectives);
    detail::hash_credit(hash, checkpoint.mechanics.participantCredit);
    detail::hash_combat(hash, checkpoint.mechanics.combat);
    return hash.finish();
}

} // namespace sunrise::server::gameplay::physics::persistence
