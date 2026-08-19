#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "internal.h"
#include "world_coordinator.h"

namespace sunrise::server::gameplay::physics::replication::coordinator_detail {

[[nodiscard]] inline bool
equal_replica(const state::gameplay::physics::PeerReplicaHandle& left,
              const state::gameplay::physics::PeerReplicaHandle& right) noexcept {
    return detail::equal_context(left.context, right.context) && left.generation == right.generation
           && left.slot == right.slot;
}

[[nodiscard]] inline bool equal_interest(const interest::PeerKey& left,
                                         const interest::PeerKey& right) noexcept {
    return left.memberId == right.memberId && left.viewGeneration == right.viewGeneration
           && left.activitySessionId == right.activitySessionId;
}

[[nodiscard]] inline bool valid_interest(const interest::PeerKey& key) noexcept {
    return key.memberId != 0 && key.viewGeneration != 0 && key.activitySessionId != 0;
}

/** @return True when one State slot still holds the exact allocation identity. */
[[nodiscard]] inline bool
exact_slot(const state::gameplay::physics::SlotSnapshot& slot,
           const state::gameplay::physics::Allocation& allocation) noexcept {
    return slot.owner == state::gameplay::physics::LeaseOwner::server
           && slot.actorId == allocation.actorId
           && slot.allocationGeneration == allocation.generation
           && slot.allocationSequence == allocation.sequence
           && slot.incarnation == allocation.token.incarnation;
}

/** @return True when no exact owner still retains one State allocation. */
[[nodiscard]] inline bool
references_empty(const state::gameplay::physics::SlotSnapshot& slot) noexcept {
    return std::all_of(slot.references.begin(), slot.references.end(), [](std::uint16_t count) {
        return count == 0;
    });
}

/** @return True when a planner value belongs to its exact registered State replica. */
[[nodiscard]] inline bool valid_planner(const PeerState& peer) noexcept {
    return peer.occupied && peer.generation != 0 && peer.generation == peer.replica.generation
           && peer.nextRecordGeneration != 0 && peer.nextContributionGeneration != 0;
}

/** @return True when a common state belongs to the bound activity and epoch pair. */
[[nodiscard]] inline bool
valid_common(const common::State& commonState,
             const state::gameplay::physics::ActivityReplicaContext& activity) noexcept {
    return commonState.phase != common::Phase::absent
           && commonState.binding.activitySessionId == activity.activitySessionId
           && commonState.binding.patchEpoch == activity.patchEpoch;
}

/** Simulates one publish without opening a duplicate State peer reference. */
[[nodiscard]] inline bool simulate_publish(PeerState& peer, const ActorSnapshot& actor) noexcept {
    if (!detail::valid_snapshot(actor)
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
    if (peer.nextRecordGeneration == 0
        || peer.nextRecordGeneration == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    for (ActorRecord& record : peer.actors) {
        if (!record.occupied) {
            record.desired = actor;
            record.generation = peer.nextRecordGeneration++;
            record.occupied = true;
            return true;
        }
    }
    return false;
}

/** Simulates one remove request without releasing its real State reference. */
[[nodiscard]] inline bool
simulate_remove(PeerState& peer, const state::gameplay::physics::Allocation& allocation) noexcept {
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
        actor = {};
    }
    return true;
}

} // namespace sunrise::server::gameplay::physics::replication::coordinator_detail
