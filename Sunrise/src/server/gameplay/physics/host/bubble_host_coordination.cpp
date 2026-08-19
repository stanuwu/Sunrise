#include "bubble_host.h"
#include "internal.h"
#include "state/gameplay/physics/runtime.h"

namespace sunrise::server::gameplay::physics::host {
namespace {

[[nodiscard]] bool equal_interest_key(const interest::PeerKey& left,
                                      const interest::PeerKey& right) noexcept {
    return left.memberId == right.memberId && left.viewGeneration == right.viewGeneration
           && left.activitySessionId == right.activitySessionId;
}

[[nodiscard]] bool
equal_replica(const state::gameplay::physics::PeerReplicaHandle& left,
              const state::gameplay::physics::PeerReplicaHandle& right) noexcept {
    return left.context.generation == right.context.generation
           && left.context.slot == right.context.slot && left.generation == right.generation
           && left.slot == right.slot;
}

/** @return True when allocator, interest, and peer identities agree. */
[[nodiscard]] bool valid_peer_binding(const BubbleHost::Storage::WorldSlot& slot,
                                      const PeerOpenRequest& request) noexcept {
    state::gameplay::physics::ActivityReplicaContext activity{};
    state::gameplay::physics::PeerReplicaSnapshot replica{};
    const world::WorldIdentity identity = slot.runner.identity();
    return request.interest.activitySessionId == identity.activitySessionId
           && request.common.activitySessionId == identity.activitySessionId
           && state::gameplay::physics::snapshot_context(request.replica.context, activity)
           && state::gameplay::physics::snapshot_peer_replica(request.replica, replica)
           && activity.activitySessionId == identity.activitySessionId
           && activity.patchEpoch == request.common.patchEpoch
           && replica.view.peer.authenticatedMemberId == request.interest.memberId
           && replica.view.viewEpoch == request.interest.viewGeneration;
}

} // namespace

namespace detail {

/** Releases every host-owned peer service before a world lifetime ends. */
bool reset_peer_services(BubbleHost::Storage::WorldSlot& slot) noexcept {
    bool clean = true;
    for (BubbleHost::Storage::PeerSlot& peer : slot.peers) {
        if (!peer.active) {
            continue;
        }
        clean = slot.interest.unbind_peer(peer.interestKey) && clean;
        clean = replication::reset_peer(peer.replication) && clean;
        common::reset(peer.common);
        peer.interestKey = {};
        peer.active = false;
        retire_handle_generation(peer.generation, peer.retired);
    }
    slot.interest.reset();
    slot.motion.reset();
    return clean;
}

} // namespace detail

/** Resolves one exact active peer slot within an already resolved world. */
std::size_t BubbleHost::find_peer(std::size_t worldSlot, HostPeerHandle handle) const noexcept {
    if (storage_ == nullptr || worldSlot >= storage_->worlds.size()
        || handle.slot >= kHostPeerCapacity || handle.generation == 0) {
        return kHostPeerCapacity;
    }
    const Storage::PeerSlot& peer = storage_->worlds[worldSlot].peers[handle.slot];
    return peer.active && peer.generation == handle.generation ? handle.slot : kHostPeerCapacity;
}

/** Binds validated common and replication state into one fixed peer slot. */
HostStatus BubbleHost::bind_peer(WorldHandle handle,
                                 const PeerOpenRequest& request,
                                 HostPeerHandle& output) noexcept {
    output = {};
    const std::size_t worldSlot = find_slot(handle);
    if (worldSlot == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    Storage::WorldSlot& slot = storage_->worlds[worldSlot];
    if (slot.inTick || !slot.healthy) {
        return HostStatus::unhealthy;
    }
    if (!valid_peer_binding(slot, request)) {
        return HostStatus::invalidArgument;
    }
    for (const Storage::PeerSlot& peer : slot.peers) {
        if (!peer.active) {
            continue;
        }
        if (equal_interest_key(peer.interestKey, request.interest)
            || equal_replica(peer.replication.replica, request.replica)) {
            return HostStatus::alreadyOpen;
        }
    }

    std::size_t peerSlot = slot.peers.size();
    for (std::size_t index = 0; index < slot.peers.size(); ++index) {
        if (!slot.peers[index].active && !slot.peers[index].retired) {
            peerSlot = index;
            break;
        }
    }
    if (peerSlot == slot.peers.size()) {
        return HostStatus::capacity;
    }

    Storage::PeerSlot& peer = slot.peers[peerSlot];
    if (!common::bind(peer.common, request.common)) {
        return HostStatus::serviceRejected;
    }
    if (!replication::initialize_peer(request.replica, peer.replication)) {
        common::reset(peer.common);
        return HostStatus::serviceRejected;
    }
    if (!slot.interest.bind_peer(request.interest)) {
        static_cast<void>(replication::reset_peer(peer.replication));
        common::reset(peer.common);
        return HostStatus::serviceRejected;
    }
    peer.interestKey = request.interest;
    peer.active = true;
    output = {peer.generation, static_cast<std::uint16_t>(peerSlot)};
    return HostStatus::success;
}

/** Releases one peer planner and invalidates its exact host handle. */
HostStatus BubbleHost::unbind_peer(WorldHandle handle, HostPeerHandle peerHandle) noexcept {
    const std::size_t worldSlot = find_slot(handle);
    if (worldSlot == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    Storage::WorldSlot& slot = storage_->worlds[worldSlot];
    const std::size_t peerSlot = find_peer(worldSlot, peerHandle);
    if (peerSlot == kHostPeerCapacity) {
        return HostStatus::staleHandle;
    }
    if (slot.inTick) {
        return HostStatus::unhealthy;
    }

    Storage::PeerSlot& peer = slot.peers[peerSlot];
    const bool interestClean = slot.interest.unbind_peer(peer.interestKey);
    const bool replicationClean = replication::reset_peer(peer.replication);
    common::reset(peer.common);
    peer.interestKey = {};
    peer.active = false;
    detail::retire_handle_generation(peer.generation, peer.retired);
    return interestClean && replicationClean ? HostStatus::success : HostStatus::unhealthy;
}

/** Borrows per-world services without exposing registry ownership. */
HostStatus BubbleHost::world_services(WorldHandle handle, WorldServiceAccess& output) noexcept {
    output = {};
    const std::size_t slot = find_slot(handle);
    if (slot == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    if (storage_->worlds[slot].inTick) {
        return HostStatus::unhealthy;
    }
    output.interest = &storage_->worlds[slot].interest;
    output.motion = &storage_->worlds[slot].motion;
    output.physics = &storage_->worlds[slot].physics;
    output.navigation = &storage_->worlds[slot].navigation;
    output.controller = &storage_->worlds[slot].builtInController;
    return HostStatus::success;
}

/** Borrows exact per-peer services without exposing peer registry storage. */
HostStatus BubbleHost::peer_services(WorldHandle handle,
                                     HostPeerHandle peer,
                                     PeerServiceAccess& output) noexcept {
    output = {};
    const std::size_t worldSlot = find_slot(handle);
    if (worldSlot == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    const std::size_t peerSlot = find_peer(worldSlot, peer);
    if (peerSlot == kHostPeerCapacity) {
        return HostStatus::staleHandle;
    }
    if (storage_->worlds[worldSlot].inTick) {
        return HostStatus::unhealthy;
    }
    output.common = &storage_->worlds[worldSlot].peers[peerSlot].common;
    output.replication = &storage_->worlds[worldSlot].peers[peerSlot].replication;
    return HostStatus::success;
}

/** Returns the fixed host-owned checkpoint store when storage exists. */
persistence::CheckpointStore* BubbleHost::checkpoint_store() noexcept {
    return storage_ == nullptr ? nullptr : &storage_->checkpoints;
}

/** Returns the immutable fixed checkpoint store when storage exists. */
const persistence::CheckpointStore* BubbleHost::checkpoint_store() const noexcept {
    return storage_ == nullptr ? nullptr : &storage_->checkpoints;
}

/** Returns the fixed host-owned command journal when storage exists. */
persistence::CommandJournal* BubbleHost::command_journal() noexcept {
    return storage_ == nullptr ? nullptr : storage_->journal.get();
}

/** Returns the immutable host-owned command journal when storage exists. */
const persistence::CommandJournal* BubbleHost::command_journal() const noexcept {
    return storage_ == nullptr ? nullptr : storage_->journal.get();
}

} // namespace sunrise::server::gameplay::physics::host
