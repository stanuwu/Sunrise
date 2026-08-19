#include <array>
#include <cstddef>
#include <span>

#include "world_coordinator.h"
#include "world_coordinator_internal.h"

namespace sunrise::server::gameplay::physics::replication {
namespace {

/** @return True when every immutable planner field matches. */
[[nodiscard]] bool same_actor(const ActorSnapshot& left, const ActorSnapshot& right) noexcept {
    return detail::equal_allocation(left.allocation, right.allocation)
           && detail::equal_versions(left.versions, right.versions)
           && left.transform.orientation == right.transform.orientation
           && left.transform.position == right.transform.position
           && left.parentActorId == right.parentActorId
           && left.streamSourceActorId == right.streamSourceActorId
           && left.authorityEpoch == right.authorityEpoch
           && left.combatRevision == right.combatRevision
           && left.contentRevision == right.contentRevision && left.bubble == right.bubble;
}

/** @return True when a registered planner still owns its exact State peer lifetime. */
[[nodiscard]] bool current_peer(const PeerState& peer, const interest::PeerKey& key) noexcept {
    state::gameplay::physics::PeerReplicaSnapshot snapshot{};
    return coordinator_detail::valid_planner(peer)
           && state::gameplay::physics::snapshot_peer_replica(peer.replica, snapshot)
           && snapshot.view.peer.authenticatedMemberId == key.memberId
           && snapshot.view.viewEpoch == key.viewGeneration;
}

/** @return True when State owner counts exactly match one planner object. */
[[nodiscard]] bool exact_planner_ownership(const PeerState& peer) noexcept {
    state::gameplay::physics::PeerReplicaSnapshot snapshot{};
    return state::gameplay::physics::snapshot_peer_replica(peer.replica, snapshot)
           && snapshot.allocationReferenceCount
                  == ::sunrise::server::gameplay::physics::replication::actor_count(peer)
           && snapshot.contributionTicketCount == contribution_count(peer);
}

} // namespace

/**
 * Registers the exact host-owned planner object used for later calls.
 * @param peer Initialized empty planner with an exact State replica.
 * @param key Host-owned interest baseline identity.
 * @return True when fixed registry capacity accepted the peer.
 */
bool WorldCoordinator::register_peer(PeerState& peer, const interest::PeerKey& key) noexcept {
    if (!healthy_ || !bound_ || !context_is_current() || !coordinator_detail::valid_interest(key)
        || key.activitySessionId != world_.activitySessionId
        || !coordinator_detail::valid_planner(peer)
        || !detail::equal_context(peer.replica.context, context_)
        || ::sunrise::server::gameplay::physics::replication::actor_count(peer) != 0
        || contribution_count(peer) != 0 || !current_peer(peer, key)) {
        return false;
    }
    std::size_t freeSlot = peers_.size();
    for (std::size_t index = 0; index < peers_.size(); ++index) {
        if (!peers_[index].occupied) {
            if (freeSlot == peers_.size()) {
                freeSlot = index;
            }
            continue;
        }
        if (peers_[index].planner == &peer
            || coordinator_detail::equal_replica(peers_[index].replica, peer.replica)
            || coordinator_detail::equal_interest(peers_[index].interest, key)) {
            return false;
        }
    }
    if (freeSlot >= peers_.size()) {
        return false;
    }
    peers_[freeSlot].planner = &peer;
    peers_[freeSlot].interest = key;
    peers_[freeSlot].replica = peer.replica;
    peers_[freeSlot].occupied = true;
    ++peerCount_;
    return true;
}

/**
 * Releases planner-owned references before its host peer lifetime ends.
 * @param peer Exact object passed to register_peer.
 * @return True when the planner and registry entry drained.
 */
bool WorldCoordinator::unregister_peer(PeerState& peer) noexcept {
    const std::size_t slot = find_peer(peer);
    state::gameplay::physics::PeerReplicaSnapshot snapshot{};
    if (!healthy_ || slot >= peers_.size()
        || !state::gameplay::physics::snapshot_peer_replica(peer.replica, snapshot)
        || !exact_planner_ownership(peer)) {
        return false;
    }
    if (!reset_peer(peer)) {
        healthy_ = false;
        return false;
    }
    peers_[slot] = {};
    --peerCount_;
    return true;
}

/**
 * Applies one host-owned interest pass in dependency-safe order.
 * @param manager Interest manager that already owns the registered key.
 * @param peer Exact registered planner object.
 * @param view Current spatial view for that peer lifetime.
 * @param output Receives transitions only after every planner mutation succeeds.
 * @return Applied, unchanged, or a fail-closed reason.
 */
PeerSyncResult WorldCoordinator::synchronize_peer(interest::InterestManager& manager,
                                                  PeerState& peer,
                                                  const interest::PeerView& view,
                                                  interest::Evaluation& output) noexcept {
    const std::size_t peerSlot = find_peer(peer);
    if (!healthy_) {
        return PeerSyncResult::invariantFailure;
    }
    if (!bound_ || peerSlot >= peers_.size() || !context_is_current()
        || !coordinator_detail::equal_interest(peers_[peerSlot].interest, view.key)
        || !current_peer(peer, view.key) || !exact_planner_ownership(peer)) {
        return PeerSyncResult::stale;
    }

    interestInputScratch_.fill({});
    auto& inputs = interestInputScratch_;
    std::size_t inputCount = 0;
    for (const ActorRecord& actor : actors_) {
        if (!actor.occupied || !actor.live) {
            continue;
        }
        if (inputCount >= inputs.size()) {
            return PeerSyncResult::capacity;
        }
        interest::ActorInput& input = inputs[inputCount++];
        input.key = actor.key;
        input.parent = actor.projection.actor.parent;
        input.position = actor.projection.actor.transform.position;
        input.bubble = actor.projection.actor.bubble;
        input.alwaysRelevant = actor.alwaysRelevant;
    }

    interestScratch_ = manager;
    evaluationScratch_ = {};
    if (!interestScratch_.evaluate(
            world_, view, std::span(inputs).first(inputCount), evaluationScratch_)) {
        return PeerSyncResult::invalid;
    }

    peerScratch_ = peer;
    PeerState& simulation = peerScratch_;
    bool changed = false;
    for (std::size_t index = 0; index < evaluationScratch_.count; ++index) {
        const interest::Transition& transition = evaluationScratch_.transitions[index];
        const std::size_t actorSlot = find_actor(transition.actor);
        if (actorSlot >= actors_.size()) {
            return PeerSyncResult::stale;
        }
        const ActorRecord& actor = actors_[actorSlot];
        if (transition.kind == interest::TransitionKind::left) {
            changed = true;
            if (!coordinator_detail::simulate_remove(simulation, actor.allocation)) {
                return PeerSyncResult::stale;
            }
            continue;
        }
        if (!actor.live) {
            return PeerSyncResult::stale;
        }
        const std::size_t existing = detail::find_actor(simulation, actor.allocation);
        changed = changed || existing >= simulation.actors.size()
                  || !same_actor(simulation.actors[existing].desired, actor.projected);
        if (!coordinator_detail::simulate_publish(simulation, actor.projected)) {
            return PeerSyncResult::capacity;
        }
    }
    if (::sunrise::server::gameplay::physics::replication::actor_count(simulation)
        > state::gameplay::physics::kPeerAllocationReferenceCapacity) {
        return PeerSyncResult::capacity;
    }

    for (std::size_t index = 0; index < evaluationScratch_.count; ++index) {
        const interest::Transition& transition = evaluationScratch_.transitions[index];
        const ActorRecord& actor = actors_[find_actor(transition.actor)];
        const bool applied = transition.kind == interest::TransitionKind::left
                                 ? request_remove(peer, actor.allocation)
                                 : publish_actor(peer, actor.projected);
        if (!applied) {
            healthy_ = false;
            return PeerSyncResult::invariantFailure;
        }
    }
    manager = interestScratch_;
    output = evaluationScratch_;
    return changed ? PeerSyncResult::applied : PeerSyncResult::unchanged;
}

/** Retires remove-queued allocations only after every exact State owner drains. */
std::size_t WorldCoordinator::retire_drained() noexcept {
    if (!healthy_ || !bound_ || !context_is_current()) {
        return 0;
    }
    for (const PeerRecord& peer : peers_) {
        if (peer.occupied
            && (peer.planner == nullptr || !current_peer(*peer.planner, peer.interest)
                || !exact_planner_ownership(*peer.planner))) {
            healthy_ = false;
            return 0;
        }
    }

    std::size_t retired = 0;
    for (ActorRecord& actor : actors_) {
        if (!actor.occupied || actor.live || !actor.removeQueued) {
            continue;
        }
        state::gameplay::physics::SlotSnapshot slot{};
        if (!state::gameplay::physics::snapshot_slot(context_, actor.allocation.token.index, slot)
            || !coordinator_detail::exact_slot(slot, actor.allocation)
            || slot.lifecycle != state::gameplay::physics::Lifecycle::removeQueued) {
            healthy_ = false;
            return retired;
        }
        if (!coordinator_detail::references_empty(slot)) {
            continue;
        }
        if (!state::gameplay::physics::settle_remove(actor.allocation)) {
            healthy_ = false;
            return retired;
        }
        actor = {};
        --actorCount_;
        ++retired;
    }
    return retired;
}

} // namespace sunrise::server::gameplay::physics::replication
