#include "world_coordinator.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "world_coordinator_internal.h"

namespace sunrise::server::gameplay::physics::replication {
namespace {

/** Fresh projection preflight uses the lowest representable allocation identity. */
constexpr std::uint64_t kProjectionAllocationGeneration = 1;
/** Native reservation sequences start at two. */
constexpr std::uint8_t kProjectionAllocationSequence = 2;

[[nodiscard]] bool
same_activity(const state::gameplay::physics::ActivityReplicaContext& left,
              const state::gameplay::physics::ActivityReplicaContext& right) noexcept {
    return left.activitySessionId == right.activitySessionId && left.role == right.role
           && left.patchEpoch == right.patchEpoch && left.clientLeaseMask == right.clientLeaseMask
           && left.serverReserveMask == right.serverReserveMask;
}

/** @return True when the snapshot header belongs to one healthy bound world. */
[[nodiscard]] bool valid_snapshot_header(const world::WorldSnapshot& snapshot,
                                         const world::WorldIdentity& expected) noexcept {
    return snapshot.healthy && world::equal_world(snapshot.identity, expected)
           && snapshot.actorCount <= snapshot.actors.size()
           && snapshot.retiredActorCount <= snapshot.retiredActors.size()
           && snapshot.commandHistoryCount <= snapshot.commandHistory.size()
           && snapshot.commandHistoryCursor <= snapshot.commandHistory.size();
}

/** @return The exact projection options or the scriptless actor-revision fallback. */
[[nodiscard]] bool projection_options(std::span<const ActorProjectionOptions> options,
                                      std::size_t index,
                                      const world::ActorSnapshot& actor,
                                      std::uint64_t& combatRevision,
                                      bool& alwaysRelevant) noexcept {
    combatRevision = actor.revision;
    alwaysRelevant = false;
    if (options.empty()) {
        return combatRevision != 0;
    }
    if (index >= options.size() || !world::equal_actor(options[index].actor, actor.key)
        || options[index].combatRevision == 0) {
        return false;
    }
    combatRevision = options[index].combatRevision;
    alwaysRelevant = options[index].alwaysRelevant;
    return true;
}

/** @return True when actor keys are in the canonical logical-id order. */
[[nodiscard]] bool canonical_actor_order(std::span<const world::ActorSnapshot> actors) noexcept {
    for (std::size_t index = 0; index < actors.size(); ++index) {
        if (!world::valid_actor(actors[index].key)
            || (index != 0 && actors[index - 1].key.id >= actors[index].key.id)) {
            return false;
        }
    }
    return true;
}

/** @return The exact actor in a canonical snapshot, or null when absent. */
[[nodiscard]] const world::ActorSnapshot*
find_snapshot_actor(std::span<const world::ActorSnapshot> actors,
                    const world::ActorKey& key) noexcept {
    const auto found = std::lower_bound(
        actors.begin(),
        actors.end(),
        key.id,
        [](const world::ActorSnapshot& actor, world::ActorId id) { return actor.key.id < id; });
    return found != actors.end() && world::equal_actor(found->key, key) ? &*found : nullptr;
}

/** @return True when every parent is absent or a same-bubble live actor. */
[[nodiscard]] bool valid_dependencies(std::span<const world::ActorSnapshot> actors) noexcept {
    for (const world::ActorSnapshot& actor : actors) {
        if (!world::valid_actor(actor.parent)) {
            if (actor.parent.id != 0 || actor.parent.generation != 0) {
                return false;
            }
            continue;
        }
        const world::ActorSnapshot* parent = find_snapshot_actor(actors, actor.parent);
        if (parent == nullptr || world::equal_actor(parent->key, actor.key)
            || parent->bubble != actor.bubble) {
            return false;
        }
    }
    return true;
}

/** Builds a structurally valid allocation used only by pure projection preflight. */
[[nodiscard]] state::gameplay::physics::Allocation
projection_allocation(state::gameplay::physics::ContextHandle context,
                      world::ActorId actorId) noexcept {
    state::gameplay::physics::Allocation allocation{};
    allocation.context = context;
    allocation.actorId = actorId;
    allocation.generation = kProjectionAllocationGeneration;
    allocation.sequence = kProjectionAllocationSequence;
    return allocation;
}

/** Returns an unexpected new allocation to quarantine or normal retirement. */
void discard_allocation(const state::gameplay::physics::Allocation& allocation) noexcept {
    using state::gameplay::physics::Lifecycle;
    state::gameplay::physics::SlotSnapshot slot{};
    if (!state::gameplay::physics::snapshot_slot(
            allocation.context, allocation.token.index, slot)) {
        return;
    }
    if (slot.lifecycle == Lifecycle::reserved) {
        static_cast<void>(state::gameplay::physics::abort_reservation(allocation));
        return;
    }
    if (slot.lifecycle == Lifecycle::createQueued || slot.lifecycle == Lifecycle::live) {
        static_cast<void>(state::gameplay::physics::queue_remove(allocation));
    }
    static_cast<void>(state::gameplay::physics::settle_remove(allocation));
}

} // namespace

/**
 * Binds one exact world to one published activity allocator.
 * @param identity Exact world and owner lifetime.
 * @param context Exact State activity context.
 * @return True when an empty coordinator accepted the binding.
 */
bool WorldCoordinator::bind(const world::WorldIdentity& identity,
                            state::gameplay::physics::ContextHandle context) noexcept {
    state::gameplay::physics::ActivityReplicaContext activity{};
    if (bound_ || !healthy_ || !world::valid_world(identity)
        || !state::gameplay::physics::snapshot_context(context, activity)
        || activity.activitySessionId != identity.activitySessionId
        || generation_ == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    context_ = context;
    activity_ = activity;
    world_ = identity;
    ++generation_;
    bound_ = true;
    return true;
}

/** @return Fixed actor slot for an exact logical generation. */
std::size_t WorldCoordinator::find_actor(const world::ActorKey& actor) const noexcept {
    for (std::size_t index = 0; index < actors_.size(); ++index) {
        if (actors_[index].occupied && world::equal_actor(actors_[index].key, actor)) {
            return index;
        }
    }
    return actors_.size();
}

/** @return Fixed actor slot for one logical id across all retained generations. */
std::size_t WorldCoordinator::find_actor_id(world::ActorId actorId) const noexcept {
    for (std::size_t index = 0; index < actors_.size(); ++index) {
        if (actors_[index].occupied && actors_[index].key.id == actorId) {
            return index;
        }
    }
    return actors_.size();
}

/** @return Registered slot for the exact planner object and replica lifetime. */
std::size_t WorldCoordinator::find_peer(const PeerState& peer) const noexcept {
    for (std::size_t index = 0; index < peers_.size(); ++index) {
        if (peers_[index].occupied && peers_[index].planner == &peer
            && coordinator_detail::equal_replica(peers_[index].replica, peer.replica)) {
            return index;
        }
    }
    return peers_.size();
}

/** @return True when the bound State context retains the same immutable lease identity. */
bool WorldCoordinator::context_is_current() const noexcept {
    state::gameplay::physics::ActivityReplicaContext activity{};
    return bound_ && state::gameplay::physics::snapshot_context(context_, activity)
           && same_activity(activity_, activity);
}

/**
 * Preflights every current allocation and all new logical ids.
 * @param newActors New exact world generations that need reservations.
 * @return True when State capacity and lifecycle make the reconcile deterministic.
 */
bool WorldCoordinator::validate_state_rows(
    std::span<const world::ActorKey> newActors) const noexcept {
    std::size_t freeCount = 0;
    for (std::size_t index = 0; index < state::gameplay::physics::kEntitySlotCount; ++index) {
        state::gameplay::physics::SlotSnapshot slot{};
        if (!state::gameplay::physics::snapshot_slot(context_, index, slot)) {
            return false;
        }
        if (slot.owner == state::gameplay::physics::LeaseOwner::server
            && slot.lifecycle == state::gameplay::physics::Lifecycle::free && slot.actorId == 0) {
            ++freeCount;
        }
        for (const world::ActorKey& actor : newActors) {
            if (slot.actorId == actor.id) {
                return false;
            }
        }
    }
    if (freeCount < newActors.size()) {
        return false;
    }
    for (const ActorRecord& actor : actors_) {
        if (!actor.occupied) {
            continue;
        }
        state::gameplay::physics::SlotSnapshot slot{};
        if (!state::gameplay::physics::snapshot_slot(context_, actor.allocation.token.index, slot)
            || !coordinator_detail::exact_slot(slot, actor.allocation)) {
            return false;
        }
        const auto expected = actor.live ? state::gameplay::physics::Lifecycle::live
                                         : state::gameplay::physics::Lifecycle::removeQueued;
        if (slot.lifecycle != expected) {
            return false;
        }
    }
    return true;
}

/**
 * Reconciles one committed world snapshot into stable allocation identities.
 * @param snapshot Canonical live world actors.
 * @param options Optional exact per-actor combat and relevance inputs.
 * @return Applied, unchanged, or a fail-closed reason.
 */
WorldReconcileResult
WorldCoordinator::reconcile(const world::WorldSnapshot& snapshot,
                            std::span<const ActorProjectionOptions> options) noexcept {
    if (!healthy_) {
        return WorldReconcileResult::invariantFailure;
    }
    if (!bound_ || !context_is_current() || !valid_snapshot_header(snapshot, world_)
        || (!options.empty() && options.size() != snapshot.actorCount)) {
        return WorldReconcileResult::invalid;
    }
    if (hasSnapshot_
        && (snapshot.tick < lastTick_
            || (snapshot.tick == lastTick_ && snapshot.canonicalHash != lastHash_))) {
        return WorldReconcileResult::stale;
    }

    const auto liveActors = std::span(snapshot.actors).first(snapshot.actorCount);
    if (!canonical_actor_order(liveActors) || !valid_dependencies(liveActors)) {
        return WorldReconcileResult::invalid;
    }

    actorScratch_ = actors_;
    seenScratch_.fill(false);
    newActorScratch_.fill({});
    newSlotScratch_.fill(0);
    allocationScratch_.fill({});
    auto& candidate = actorScratch_;
    auto& seen = seenScratch_;
    auto& newActors = newActorScratch_;
    auto& newSlots = newSlotScratch_;
    std::size_t newCount = 0;
    std::size_t candidateCount = actorCount_;
    bool changed = !hasSnapshot_;

    for (std::size_t index = 0; index < liveActors.size(); ++index) {
        const world::ActorSnapshot& actor = liveActors[index];
        std::uint64_t combatRevision = 0;
        bool alwaysRelevant = false;
        if (!projection_options(options, index, actor, combatRevision, alwaysRelevant)) {
            return WorldReconcileResult::invalid;
        }
        std::size_t slot = find_actor(actor.key);
        if (slot >= actors_.size()) {
            if (find_actor_id(actor.key.id) < actors_.size()
                || candidateCount >= candidate.size()) {
                return WorldReconcileResult::capacity;
            }
            for (slot = 0; slot < candidate.size() && candidate[slot].occupied; ++slot) {}
            if (slot >= candidate.size()) {
                return WorldReconcileResult::capacity;
            }
            WorldProjectionState preflight{};
            ActorSnapshot projected{};
            if (project_actor(world_,
                              actor,
                              projection_allocation(context_, actor.key.id),
                              combatRevision,
                              preflight,
                              projected)
                != ProjectionResult::created) {
                return WorldReconcileResult::invalid;
            }
            candidate[slot] = {};
            candidate[slot].key = actor.key;
            candidate[slot].alwaysRelevant = alwaysRelevant;
            candidate[slot].occupied = true;
            candidate[slot].live = true;
            newActors[newCount] = actor.key;
            newSlots[newCount] = slot;
            ++newCount;
            ++candidateCount;
            changed = true;
        } else {
            if (!candidate[slot].live) {
                return WorldReconcileResult::stale;
            }
            ActorSnapshot projected = candidate[slot].projected;
            const ProjectionResult result = project_actor(world_,
                                                          actor,
                                                          candidate[slot].allocation,
                                                          combatRevision,
                                                          candidate[slot].projection,
                                                          projected);
            if (result == ProjectionResult::invalid) {
                return WorldReconcileResult::invalid;
            }
            changed = changed || result == ProjectionResult::updated
                      || candidate[slot].alwaysRelevant != alwaysRelevant;
            candidate[slot].projected = projected;
            candidate[slot].alwaysRelevant = alwaysRelevant;
        }
        seen[slot] = true;
    }

    for (std::size_t index = 0; index < candidate.size(); ++index) {
        if (candidate[index].occupied && candidate[index].live && !seen[index]) {
            candidate[index].live = false;
            candidate[index].removeQueued = true;
            changed = true;
        }
    }
    if (!validate_state_rows(std::span(newActors).first(newCount))) {
        return WorldReconcileResult::capacity;
    }

    auto& reservations = allocationScratch_;
    std::size_t reservationCount = 0;
    for (std::size_t index = 0; index < newCount; ++index) {
        state::gameplay::physics::Allocation allocation{};
        if (!state::gameplay::physics::reserve(context_, newActors[index].id, allocation)
            || !state::gameplay::physics::queue_create(allocation)
            || !state::gameplay::physics::commit_create(allocation)) {
            discard_allocation(allocation);
            for (std::size_t prior = 0; prior < reservationCount; ++prior) {
                discard_allocation(reservations[prior]);
            }
            healthy_ = false;
            return WorldReconcileResult::invariantFailure;
        }
        reservations[reservationCount++] = allocation;
        ActorRecord& record = candidate[newSlots[index]];
        const world::ActorSnapshot* actor = find_snapshot_actor(liveActors, record.key);
        std::uint64_t combatRevision = 0;
        bool alwaysRelevant = false;
        if (actor == nullptr
            || !projection_options(options,
                                   static_cast<std::size_t>(actor - liveActors.data()),
                                   *actor,
                                   combatRevision,
                                   alwaysRelevant)
            || project_actor(
                   world_, *actor, allocation, combatRevision, record.projection, record.projected)
                   != ProjectionResult::created) {
            for (std::size_t prior = 0; prior < reservationCount; ++prior) {
                discard_allocation(reservations[prior]);
            }
            healthy_ = false;
            return WorldReconcileResult::invariantFailure;
        }
        record.allocation = allocation;
    }

    for (std::size_t index = 0; index < candidate.size(); ++index) {
        if (actors_[index].occupied && actors_[index].live && !candidate[index].live
            && !state::gameplay::physics::queue_remove(candidate[index].allocation)) {
            for (std::size_t prior = 0; prior < reservationCount; ++prior) {
                discard_allocation(reservations[prior]);
            }
            healthy_ = false;
            return WorldReconcileResult::invariantFailure;
        }
    }

    actors_ = candidate;
    actorCount_ = candidateCount;
    lastTick_ = snapshot.tick;
    lastHash_ = snapshot.canonicalHash;
    hasSnapshot_ = true;
    return changed ? WorldReconcileResult::applied : WorldReconcileResult::unchanged;
}

/** Copies one stable allocation without changing output on failure. */
bool WorldCoordinator::allocation_for(const world::ActorKey& actor,
                                      state::gameplay::physics::Allocation& output) const noexcept {
    const std::size_t slot = find_actor(actor);
    if (slot >= actors_.size()) {
        return false;
    }
    output = actors_[slot].allocation;
    return true;
}

bool WorldCoordinator::bound() const noexcept {
    return bound_;
}

std::size_t WorldCoordinator::actor_count() const noexcept {
    return actorCount_;
}

std::size_t WorldCoordinator::peer_count() const noexcept {
    return peerCount_;
}

bool WorldCoordinator::healthy() const noexcept {
    return healthy_;
}

} // namespace sunrise::server::gameplay::physics::replication
