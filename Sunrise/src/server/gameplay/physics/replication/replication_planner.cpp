#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "internal.h"
#include "runtime.h"

namespace sunrise::server::gameplay::physics::replication {

/** Initializes one empty per-peer planner. */
bool initialize_peer(state::gameplay::physics::PeerReplicaHandle replica,
                     PeerState& peer) noexcept {
    state::gameplay::physics::PeerReplicaSnapshot snapshot{};
    if (peer.occupied || !state::gameplay::physics::snapshot_peer_replica(replica, snapshot)) {
        return false;
    }
    SecureZeroMemory(&peer, sizeof peer);
    peer.replica = replica;
    peer.generation = replica.generation;
    peer.nextRecordGeneration = 1;
    peer.nextContributionGeneration = 1;
    peer.occupied = true;
    return true;
}

/** Releases every planner-owned ticket and actor reference. */
bool reset_peer(PeerState& peer) noexcept {
    if (!peer.occupied) {
        return false;
    }
    bool clean = true;
    for (InFlightRecord& contribution : peer.inFlight) {
        if (contribution.occupied) {
            clean =
                state::gameplay::physics::settle_contribution_ticket(contribution.ticket) && clean;
        }
    }
    for (ActorRecord& actor : peer.actors) {
        if (actor.occupied) {
            clean =
                state::gameplay::physics::release_for_peer(peer.replica, actor.desired.allocation)
                && clean;
        }
    }
    SecureZeroMemory(&peer, sizeof peer);
    return clean;
}

/** Adds one relevant actor or publishes its newest immutable state. */
bool publish_actor(PeerState& peer, const ActorSnapshot& actor) noexcept {
    if (!peer.occupied || !detail::valid_snapshot(actor)
        || !detail::equal_context(peer.replica.context, actor.allocation.context)) {
        return false;
    }
    const std::size_t existing = detail::find_actor(peer, actor.allocation);
    if (existing < peer.actors.size()) {
        ActorRecord& record = peer.actors[existing];
        if (record.removeRequested || !detail::monotonic(record.desired.versions, actor.versions)
            || !detail::consistent_update(record.desired, actor)) {
            return false;
        }
        record.desired = actor;
        return true;
    }
    std::uint64_t generation = 0;
    if (!detail::claim_generation(peer.nextRecordGeneration, generation)
        || !state::gameplay::physics::retain_for_peer(peer.replica, actor.allocation)) {
        return false;
    }
    for (ActorRecord& record : peer.actors) {
        if (!record.occupied) {
            record.desired = actor;
            record.generation = generation;
            record.occupied = true;
            return true;
        }
    }
    static_cast<void>(state::gameplay::physics::release_for_peer(peer.replica, actor.allocation));
    return false;
}

/** Marks one actor for a strict remove after its create may have landed. */
bool request_remove(PeerState& peer,
                    const state::gameplay::physics::Allocation& allocation) noexcept {
    const std::size_t slot = detail::find_actor(peer, allocation);
    if (slot >= peer.actors.size()) {
        return false;
    }
    ActorRecord& actor = peer.actors[slot];
    if (actor.removeRequested) {
        return true;
    }
    actor.removeRequested = true;
    if (!actor.createCommitted && !actor.contributionPending) {
        const bool released =
            state::gameplay::physics::release_for_peer(peer.replica, actor.desired.allocation);
        SecureZeroMemory(&actor, sizeof actor);
        return released;
    }
    return true;
}

/** Selects the highest-priority actor contribution without mutating baselines. */
PrepareResult prepare_next(PeerState& peer, Plan& plan) noexcept {
    plan = {};
    plan.actorSlot = kActorCapacity;
    if (!peer.occupied) {
        return PrepareResult::invalid;
    }
    std::size_t selected = peer.actors.size();
    Operation selectedOperation = Operation::none;
    for (std::size_t index = 0; index < peer.actors.size(); ++index) {
        const ActorRecord& actor = peer.actors[index];
        if (!actor.occupied) {
            continue;
        }
        const Operation operation = detail::pending_operation(actor);
        if (operation == Operation::none || !detail::causal_ready(peer, actor, operation)) {
            continue;
        }
        if (selected == peer.actors.size()
            || detail::operation_priority(operation) < detail::operation_priority(selectedOperation)
            || (detail::operation_priority(operation)
                    == detail::operation_priority(selectedOperation)
                && actor.desired.allocation.actorId
                       < peer.actors[selected].desired.allocation.actorId)) {
            selected = index;
            selectedOperation = operation;
        }
    }
    if (selected == peer.actors.size()) {
        return PrepareResult::idle;
    }
    if (std::find_if(peer.inFlight.begin(),
                     peer.inFlight.end(),
                     [](const InFlightRecord& record) { return !record.occupied; })
        == peer.inFlight.end()) {
        return PrepareResult::full;
    }
    const ActorRecord& actor = peer.actors[selected];
    plan.actor = actor.desired;
    plan.peerGeneration = peer.generation;
    plan.recordGeneration = actor.generation;
    plan.fields =
        selectedOperation == Operation::create ? kCreateFieldMask : detail::dirty_fields(actor);
    plan.operation = selectedOperation;
    plan.actorSlot = selected;
    return PrepareResult::ready;
}

} // namespace sunrise::server::gameplay::physics::replication
