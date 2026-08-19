#include "checkpoint_hash_internal.h"

namespace sunrise::server::gameplay::physics::persistence::detail {
namespace {

/** Appends one complete generic mechanics command-deduplication window. */
void hash_commands(CheckpointHashWriter& hash,
                   const mechanics::CommandDeduplicatorSnapshot& snapshot) noexcept {
    hash.scalar(snapshot.count);
    for (const mechanics::CommandRecord& command : snapshot.records) {
        hash.scalar(command.commandId);
        hash.scalar(command.executionTick);
        hash.scalar(command.policyOwnerId);
        hash.scalar(command.occupied);
    }
}

/** Appends one generation-checked trigger or timer handle. */
template <typename Handle> void hash_handle(CheckpointHashWriter& hash, Handle handle) noexcept {
    hash.scalar(handle.generation);
    hash.scalar(handle.slot);
}

/** Appends one generation-checked actor identity. */
void hash_actor(CheckpointHashWriter& hash, mechanics::ActorHandle actor) noexcept {
    hash.scalar(actor.actorId);
    hash.scalar(actor.generation);
}

} // namespace

/** Appends every timer slot, generation marker, and retained command. */
void hash_timers(CheckpointHashWriter& hash,
                 const mechanics::TickTimerServiceSnapshot& snapshot) noexcept {
    hash.scalar(snapshot.worldEpoch);
    for (const mechanics::TickTimerSlotSnapshot& timer : snapshot.slots) {
        hash.scalar(timer.generation);
        hash.scalar(timer.timerId);
        hash.scalar(timer.fireTick);
        hash.scalar(timer.policyOwnerId);
        hash.scalar(timer.scheduleCommandId);
        hash.scalar(timer.active);
    }
    hash_commands(hash, snapshot.commands);
}

/** Appends every trigger, exact occupancy, and evaluation boundary. */
void hash_triggers(CheckpointHashWriter& hash,
                   const mechanics::TriggerSystemSnapshot& snapshot) noexcept {
    hash.scalar(snapshot.worldEpoch);
    hash.scalar(snapshot.lastEvaluationTick);
    hash.scalar(snapshot.membershipCount);
    hash.scalar(snapshot.hasEvaluation);
    for (const mechanics::TriggerSlotSnapshot& trigger : snapshot.slots) {
        hash.scalar(trigger.generation);
        hash.scalar(trigger.triggerId);
        hash.scalar(trigger.policyOwnerId);
        hash.scalar(trigger.createCommandId);
        hash.scalar(trigger.bubble);
        hash.scalar(trigger.active);
    }
    for (const mechanics::TriggerOccupancy& occupancy : snapshot.memberships) {
        hash_handle(hash, occupancy.trigger);
        hash_actor(hash, occupancy.actor);
    }
    hash_commands(hash, snapshot.commands);
}

/** Appends every objective definition, current value, and generation marker. */
void hash_objectives(CheckpointHashWriter& hash,
                     const mechanics::ObjectiveLedgerSnapshot& snapshot) noexcept {
    hash.scalar(snapshot.worldEpoch);
    for (const mechanics::ObjectiveCounterSnapshot& counter : snapshot.counters) {
        for (const std::int64_t threshold : counter.definition.thresholds) {
            hash.scalar(threshold);
        }
        hash.scalar(counter.definition.counterId);
        hash.scalar(counter.definition.policyOwnerId);
        hash.scalar(counter.definition.initialValue);
        hash.scalar(counter.definition.minimumValue);
        hash.scalar(counter.definition.maximumValue);
        hash.scalar(counter.definition.thresholdCount);
        hash.scalar(counter.definition.policy);
        hash.scalar(counter.generation);
        hash.scalar(counter.revision);
        hash.scalar(counter.value);
        hash.scalar(counter.active);
    }
    hash_commands(hash, snapshot.commands);
}

/** Appends every participant definition, channel value, and presence boundary. */
void hash_credit(CheckpointHashWriter& hash,
                 const mechanics::ParticipantCreditLedgerSnapshot& snapshot) noexcept {
    hash.scalar(snapshot.worldEpoch);
    for (const mechanics::ParticipantCreditSnapshot& participant : snapshot.participants) {
        for (const std::uint32_t channel : participant.definition.channelIds) {
            hash.scalar(channel);
        }
        hash.scalar(participant.definition.participantId);
        hash.scalar(participant.definition.policyOwnerId);
        hash.scalar(participant.definition.channelCount);
        for (const mechanics::ParticipantCreditChannel& channel : participant.channels) {
            hash.scalar(channel.value);
            hash.scalar(channel.channelId);
        }
        hash.scalar(participant.generation);
        hash.scalar(participant.revision);
        hash.scalar(participant.firstEligibleTick);
        hash.scalar(participant.lastEligibleTick);
        hash.scalar(participant.presenceTicks);
        hash.scalar(participant.active);
        hash.scalar(participant.hasPresence);
    }
    hash_commands(hash, snapshot.commands);
}

} // namespace sunrise::server::gameplay::physics::persistence::detail
