#include <limits>

#include "../world/actor_store.h"
#include "checkpoint.h"

namespace sunrise::server::gameplay::physics::persistence {
namespace {

/** @return True when ActorStore accepts exact live and retired state. */
[[nodiscard]] bool valid_actors(const world::WorldSnapshot& snapshot) noexcept {
    if (snapshot.actorCount > snapshot.actors.size()
        || snapshot.retiredActorCount > snapshot.retiredActors.size()) {
        return false;
    }
    const auto actors = std::span(snapshot.actors).first(snapshot.actorCount);
    const auto retired = std::span(snapshot.retiredActors).first(snapshot.retiredActorCount);
    if (!world::ActorStore::valid_restore(actors, retired)) {
        return false;
    }
    const world::TickId maximumValidFrom =
        snapshot.tick == (std::numeric_limits<world::TickId>::max)() ? snapshot.tick
                                                                     : snapshot.tick + 1U;
    for (const world::ActorSnapshot& actor : actors) {
        if (actor.authority.validFromTick > maximumValidFrom) {
            return false;
        }
    }
    for (const world::RetiredActorGeneration& entry : retired) {
        if (entry.retiredTick > snapshot.tick) {
            return false;
        }
    }
    return true;
}

/** @return True when the world dedupe ring is exact, unique, and causal. */
[[nodiscard]] bool valid_command_history(const world::WorldSnapshot& snapshot) noexcept {
    if (snapshot.commandHistoryCount > snapshot.commandHistory.size()
        || snapshot.commandHistoryCursor != 0) {
        return false;
    }
    for (std::size_t index = 0; index < snapshot.commandHistoryCount; ++index) {
        const world::HostCommandHeader& command = snapshot.commandHistory[index];
        if (!world::valid_command_header(command)
            || !world::equal_world(command.world, snapshot.identity)
            || command.executeTick > snapshot.tick) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            const world::HostCommandHeader& seen = snapshot.commandHistory[prior];
            if (seen.source.kind == command.source.kind && seen.source.id == command.source.id
                && seen.commandId == command.commandId) {
                return false;
            }
        }
    }
    return true;
}

/** @return True when the timer service accepts the exact retained snapshot. */
[[nodiscard]] bool valid_timers(const mechanics::TickTimerServiceSnapshot& snapshot) noexcept {
    mechanics::TickTimerService service{};
    return service.restore(snapshot);
}

/** @return True when the trigger service accepts the exact retained snapshot. */
[[nodiscard]] bool valid_triggers(const mechanics::TriggerSystemSnapshot& snapshot) noexcept {
    mechanics::TriggerSystem service{};
    return service.restore(snapshot);
}

/** @return True when the objective service accepts the exact retained snapshot. */
[[nodiscard]] bool valid_objectives(const mechanics::ObjectiveLedgerSnapshot& snapshot) noexcept {
    mechanics::ObjectiveLedger service{};
    return service.restore(snapshot);
}

/** @return True when the credit service accepts the exact retained snapshot. */
[[nodiscard]] bool
valid_credit(const mechanics::ParticipantCreditLedgerSnapshot& snapshot) noexcept {
    mechanics::ParticipantCreditLedger service{};
    return service.restore(snapshot);
}

/** @return True when the combat service accepts the exact retained snapshot. */
[[nodiscard]] bool valid_combat(const mechanics::CombatKernelSnapshot& snapshot) noexcept {
    mechanics::CombatKernel service{};
    return service.restore(snapshot);
}

/** @return True when every mechanics service accepts the exact retained snapshot. */
[[nodiscard]] bool valid_mechanics(const MechanicsCheckpoint& state) noexcept {
    const mechanics::WorldEpoch epoch = state.worldEpoch;
    return epoch != mechanics::kAbsentWorldEpoch && state.timers.worldEpoch == epoch
           && state.triggers.worldEpoch == epoch && state.objectives.worldEpoch == epoch
           && state.participantCredit.worldEpoch == epoch && state.combat.worldEpoch == epoch
           && valid_timers(state.timers) && valid_triggers(state.triggers)
           && valid_objectives(state.objectives) && valid_credit(state.participantCredit)
           && valid_combat(state.combat);
}

} // namespace

/** Validates one logical checkpoint without consulting live state. */
bool valid_checkpoint(const Checkpoint& checkpoint) noexcept {
    return checkpoint.checkpointId != 0 && checkpoint.logicalWorldId != 0
           && checkpoint.contentBuildId != 0 && checkpoint.schemaVersion != 0
           && checkpoint.fixedRateHz != 0 && checkpoint.fixedRateHz <= world::kMaximumFixedRateHz
           && checkpoint.policyState.size <= checkpoint.policyState.bytes.size()
           && world::valid_blueprint(checkpoint.policy)
           && world::valid_world(checkpoint.world.identity) && checkpoint.world.healthy
           && checkpoint.world.canonicalHash != 0 && checkpoint.canonicalHash != 0
           && valid_actors(checkpoint.world) && valid_command_history(checkpoint.world)
           && checkpoint.world.canonicalHash == world::compute_snapshot_hash(checkpoint.world)
           && valid_mechanics(checkpoint.mechanics)
           && checkpoint.canonicalHash == compute_checkpoint_hash(checkpoint);
}

/** Rebinds only logical owner epochs for a cold process lifetime. */
bool prepare_recovery(const Checkpoint& checkpoint,
                      std::uint64_t newWorldGeneration,
                      std::uint64_t newOwnerGeneration,
                      mechanics::WorldEpoch newMechanicsEpoch,
                      RecoveryImage& image) noexcept {
    image = {};
    if (!valid_checkpoint(checkpoint)
        || newWorldGeneration <= checkpoint.world.identity.worldGeneration
        || newOwnerGeneration <= checkpoint.world.identity.ownerGeneration
        || newMechanicsEpoch <= checkpoint.mechanics.worldEpoch) {
        return false;
    }
    image.checkpoint = checkpoint;
    image.checkpoint.world.identity.worldGeneration = newWorldGeneration;
    image.checkpoint.world.identity.ownerGeneration = newOwnerGeneration;
    for (std::size_t index = 0; index < image.checkpoint.world.commandHistoryCount; ++index) {
        image.checkpoint.world.commandHistory[index].world = image.checkpoint.world.identity;
    }
    image.checkpoint.mechanics.worldEpoch = newMechanicsEpoch;
    image.checkpoint.mechanics.timers.worldEpoch = newMechanicsEpoch;
    image.checkpoint.mechanics.triggers.worldEpoch = newMechanicsEpoch;
    image.checkpoint.mechanics.objectives.worldEpoch = newMechanicsEpoch;
    image.checkpoint.mechanics.participantCredit.worldEpoch = newMechanicsEpoch;
    image.checkpoint.mechanics.combat.worldEpoch = newMechanicsEpoch;
    image.checkpoint.world.canonicalHash = 0;
    image.checkpoint.canonicalHash = 0;
    image.requiresCanonicalHash = true;
    return true;
}

} // namespace sunrise::server::gameplay::physics::persistence
