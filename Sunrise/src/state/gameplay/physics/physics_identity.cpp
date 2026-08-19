#include "physics_identity.h"

#include <cstddef>
#include <cstdint>

#include "runtime.h"

namespace sunrise::state::gameplay::physics {
namespace {

/** The raw reservation byte skips zero and one after wrap. */
constexpr std::uint8_t kFirstAllocationSequence = 2;

} // namespace

/** Returns whether one logical entity index is present in the lease mask. */
bool mask_has(const activity::entity_slots::LeaseMask& mask, std::size_t index) noexcept {
    /** Entity lease masks store eight consecutive logical slots in each byte. */
    constexpr std::size_t kBitsPerByte = 8;
    const std::size_t byte = index / kBitsPerByte;
    const unsigned bit = static_cast<unsigned>(index % kBitsPerByte);
    return (std::to_integer<unsigned>(mask[byte]) & (1U << bit)) != 0;
}

/**
 * Checks that the committed client and server leases are disjoint.
 * @param context Candidate activity context.
 * @return True when no index belongs to both owners.
 */
bool masks_are_disjoint(const ActivityReplicaContext& context) noexcept {
    for (std::size_t index = 0; index < context.clientLeaseMask.size(); ++index) {
        if ((context.clientLeaseMask[index] & context.serverReserveMask[index]) != std::byte{}) {
            return false;
        }
    }
    return true;
}

/** Returns whether the role names a publishable activity context. */
bool valid_role(ReplicaRole role) noexcept {
    return role == ReplicaRole::privateActivity || role == ReplicaRole::currentActivity
           || role == ReplicaRole::publicTarget;
}

/** Returns whether every authenticated peer generation field matches. */
bool equal_peer_key(const PeerKey& left, const PeerKey& right) noexcept {
    return left.authenticatedMemberId == right.authenticatedMemberId
           && left.associationEpoch == right.associationEpoch
           && left.remoteConnectionSequence == right.remoteConnectionSequence
           && left.localConnectionSequence == right.localConnectionSequence
           && left.channelEpoch == right.channelEpoch;
}

/** Returns whether the peer, group, and view epoch match. */
bool equal_view_key(const ViewKey& left, const ViewKey& right) noexcept {
    return equal_peer_key(left.peer, right.peer) && left.groupSessionId == right.groupSessionId
           && left.viewEpoch == right.viewEpoch;
}

/** Returns whether every required peer and view identity field is present. */
bool valid_view_key(const ViewKey& view) noexcept {
    return view.peer.authenticatedMemberId != kAbsentMemberId
           && view.peer.associationEpoch != kAbsentAssociationEpoch
           && view.peer.channelEpoch != kAbsentChannelEpoch
           && view.groupSessionId != kAbsentGroupSessionId && view.viewEpoch != kAbsentViewEpoch;
}

/** Returns whether the reference kind is owned outside peer state. */
bool valid_host_reference_kind(ReferenceKind kind) noexcept {
    return kind == ReferenceKind::dependency || kind == ReferenceKind::command;
}

/** Returns whether the contribution kind owns a packet or replay reference. */
bool valid_contribution_kind(ContributionKind kind) noexcept {
    return kind == ContributionKind::packet || kind == ContributionKind::replay;
}

/** Returns the aggregate reference kind owned by the contribution. */
ReferenceKind contribution_reference_kind(ContributionKind kind) noexcept {
    return kind == ContributionKind::packet ? ReferenceKind::packet : ReferenceKind::replay;
}

/** Returns the fixed aggregate-counter index for the reference kind. */
std::size_t reference_index(ReferenceKind kind) noexcept {
    return static_cast<std::size_t>(kind);
}

/** Returns whether both handles name one exact context generation. */
bool equal_context_handle(ContextHandle left, ContextHandle right) noexcept {
    return left.generation == right.generation && left.slot == right.slot;
}

/** Returns whether both views have the same peer and group owner. */
bool equal_view_owner(const ViewKey& left, const ViewKey& right) noexcept {
    return equal_peer_key(left.peer, right.peer) && left.groupSessionId == right.groupSessionId;
}

/** Returns whether every internal allocation identity field matches. */
bool equal_allocation(const Allocation& left, const Allocation& right) noexcept {
    return left.context.generation == right.context.generation
           && left.context.slot == right.context.slot && left.token.index == right.token.index
           && left.token.incarnation == right.token.incarnation && left.actorId == right.actorId
           && left.generation == right.generation && left.sequence == right.sequence;
}

/**
 * Advances one raw reservation sequence without producing zero or one.
 * @param oldSequence Stored raw reservation byte.
 * @return Exact next reservation byte.
 */
std::uint8_t next_allocation_sequence(std::uint8_t oldSequence) noexcept {
    if (oldSequence == 0) {
        return kFirstAllocationSequence;
    }
    const std::uint8_t next = static_cast<std::uint8_t>(oldSequence + 1U);
    return next == 0 ? kFirstAllocationSequence : next;
}

} // namespace sunrise::state::gameplay::physics
