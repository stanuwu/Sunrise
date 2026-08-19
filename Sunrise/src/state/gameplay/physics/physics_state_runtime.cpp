#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "physics_identity.h"
#include "runtime.h"

namespace sunrise::state::gameplay::physics {
namespace {

/** The 4-bit incarnation mask wraps 15 back to zero. */
constexpr std::uint8_t kWireIncarnationMask = kWireIncarnationCount - 1;
/** The first process-local handle generation follows the cleared zero sentinel. */
constexpr std::uint64_t kFirstHandleGeneration = 1;

/** One fixed allocator record. Exact owners live in context or peer ticket tables. */
struct SlotRecord final {
    std::uint64_t actorId{};
    std::uint64_t allocationGeneration{};
    std::uint8_t allocationSequence{};
    std::uint8_t incarnation{};
    std::uint8_t incarnationUseCount{};
    LeaseOwner owner{LeaseOwner::free};
    Lifecycle lifecycle{Lifecycle::absent};
    QuarantineReason quarantineReason{QuarantineReason::none};
};

/** One exact allocation retained by one peer replica. */
struct PeerAllocationReferenceRecord final {
    Allocation allocation{};
    bool occupied{};
};

/** One exact packet or replay reference retained by one peer replica. */
struct ContributionTicketRecord final {
    Allocation allocation{};
    std::uint64_t generation{};
    ContributionKind kind{ContributionKind::packet};
    bool occupied{};
};

/** One exact dependency or command reference retained by the activity. */
struct HostReferenceTicketRecord final {
    Allocation allocation{};
    std::uint64_t generation{};
    ReferenceKind kind{ReferenceKind::dependency};
    bool occupied{};
};

/** One peer-owned view with bounded allocation references and tickets. */
struct PeerReplicaRecord final {
    std::array<PeerAllocationReferenceRecord, kPeerAllocationReferenceCapacity> references{};
    std::array<ContributionTicketRecord, kContributionTicketCapacity> tickets{};
    ViewKey view{};
    std::uint64_t generation{};
    bool occupied{};
};

/** One activity identity, its allocator, and every peer-owned view. */
struct ContextRecord final {
    std::array<SlotRecord, kEntitySlotCount> slots{};
    std::array<PeerReplicaRecord, kPeerReplicaCapacity> peers{};
    std::array<HostReferenceTicketRecord, kHostReferenceTicketCapacity> hostTickets{};
    ActivityReplicaContext context{};
    std::uint64_t generation{};
    std::uint64_t reuseEpoch{};
    bool occupied{};
};

/** State-owned fixed storage and its publication lock. */
struct Storage final {
    SRWLOCK lock{SRWLOCK_INIT};
    std::array<ContextRecord, kReplicaContextCapacity> contexts{};
    std::uint64_t nextHandleGeneration{kFirstHandleGeneration};
    bool ready{};
};

Storage g_storage;

/** Clears all records but preserves the process-local handle sequence and Windows lock. */
void clear_storage() noexcept {
    SecureZeroMemory(g_storage.contexts.data(), sizeof g_storage.contexts);
    g_storage.ready = false;
}

/**
 * Claims one process-local generation without ever wrapping to a reusable value.
 * @param generation Receives the claimed nonzero generation.
 * @return True when the generation space is not exhausted.
 */
[[nodiscard]] bool claim_handle_generation(std::uint64_t& generation) noexcept {
    generation = kAbsentHandleGeneration;
    if (g_storage.nextHandleGeneration == kAbsentHandleGeneration) {
        return false;
    }
    generation = g_storage.nextHandleGeneration;
    if (g_storage.nextHandleGeneration == (std::numeric_limits<std::uint64_t>::max)()) {
        g_storage.nextHandleGeneration = kAbsentHandleGeneration;
    } else {
        ++g_storage.nextHandleGeneration;
    }
    return true;
}

[[nodiscard]] ContextHandle make_context_handle(const ContextRecord& record,
                                                std::size_t slot) noexcept {
    return ContextHandle{record.generation, slot};
}

/**
 * Resolves one mutable context while the storage lock is held.
 * @param handle Generation-checked context handle.
 * @return Matching occupied record, otherwise null.
 */
[[nodiscard]] ContextRecord* resolve_context(ContextHandle handle) noexcept {
    if (!g_storage.ready || handle.slot >= g_storage.contexts.size()
        || handle.generation == kAbsentHandleGeneration) {
        return nullptr;
    }
    ContextRecord& record = g_storage.contexts[handle.slot];
    return record.occupied && record.generation == handle.generation ? &record : nullptr;
}

/**
 * Resolves one immutable context while the storage lock is held.
 * @param handle Generation-checked context handle.
 * @return Matching occupied record, otherwise null.
 */
[[nodiscard]] const ContextRecord* resolve_context_const(ContextHandle handle) noexcept {
    return resolve_context(handle);
}

/**
 * Compares a retained allocation with the current or quarantined slot identity.
 * @param slot Candidate allocator slot.
 * @param index Candidate allocator index.
 * @param allocation Exact retained allocation.
 * @return True when the identities match.
 */
[[nodiscard]] bool allocation_matches_slot(const SlotRecord& slot,
                                           std::size_t index,
                                           const Allocation& allocation) noexcept {
    if (allocation.actorId == kAbsentActorId || allocation.token.index != index
        || slot.owner != LeaseOwner::server || slot.lifecycle == Lifecycle::absent
        || slot.lifecycle == Lifecycle::free || slot.allocationGeneration != allocation.generation
        || slot.allocationSequence != allocation.sequence
        || slot.incarnation != allocation.token.incarnation) {
        return false;
    }
    return slot.actorId == allocation.actorId;
}

/**
 * Resolves an allocation against the current slot occupant or its retired identity.
 * @param record Owning context.
 * @param allocation Exact internal allocation identity.
 * @return Matching slot, otherwise null.
 */
[[nodiscard]] SlotRecord* resolve_allocation(ContextRecord& record,
                                             const Allocation& allocation) noexcept {
    const std::size_t index = allocation.token.index;
    if (index >= record.slots.size()) {
        return nullptr;
    }
    SlotRecord& slot = record.slots[index];
    return allocation_matches_slot(slot, index, allocation) ? &slot : nullptr;
}

/**
 * Resolves the allocation and its owning context while the storage lock is held.
 * @param allocation Exact internal allocation identity.
 * @param record Receives the matching context.
 * @return Matching slot, otherwise null.
 */
[[nodiscard]] SlotRecord* resolve_allocation(const Allocation& allocation,
                                             ContextRecord*& record) noexcept {
    record = resolve_context(allocation.context);
    return record == nullptr ? nullptr : resolve_allocation(*record, allocation);
}

/**
 * Resolves one peer replica while the storage lock is held.
 * @param replica Generation-checked peer replica handle.
 * @param context Receives its exact activity context.
 * @return Matching peer record, otherwise null.
 */
[[nodiscard]] PeerReplicaRecord* resolve_peer(PeerReplicaHandle replica,
                                              ContextRecord*& context) noexcept {
    context = resolve_context(replica.context);
    if (context == nullptr || replica.slot >= context->peers.size()
        || replica.generation == kAbsentHandleGeneration) {
        return nullptr;
    }
    PeerReplicaRecord& peer = context->peers[replica.slot];
    return peer.occupied && peer.generation == replica.generation ? &peer : nullptr;
}

/**
 * Resolves one immutable peer replica while the storage lock is held.
 * @param replica Generation-checked peer replica handle.
 * @param context Receives its exact activity context.
 * @return Matching peer record, otherwise null.
 */
[[nodiscard]] const PeerReplicaRecord* resolve_peer_const(PeerReplicaHandle replica,
                                                          const ContextRecord*& context) noexcept {
    ContextRecord* mutableContext = nullptr;
    PeerReplicaRecord* mutablePeer = resolve_peer(replica, mutableContext);
    context = mutableContext;
    return mutablePeer;
}

/**
 * Builds a checked handle for one peer record.
 * @param context Owning context handle.
 * @param peer Peer record.
 * @param slot Fixed peer slot.
 * @return Exact peer replica handle.
 */
[[nodiscard]] PeerReplicaHandle
make_peer_handle(ContextHandle context, const PeerReplicaRecord& peer, std::size_t slot) noexcept {
    return PeerReplicaHandle{context, peer.generation, slot};
}

/**
 * Finds one exact peer-owned allocation reference.
 * @param peer Owning peer replica.
 * @param allocation Exact allocation identity.
 * @return Matching record, otherwise null.
 */
[[nodiscard]] PeerAllocationReferenceRecord*
find_peer_reference(PeerReplicaRecord& peer, const Allocation& allocation) noexcept {
    for (PeerAllocationReferenceRecord& reference : peer.references) {
        if (reference.occupied && equal_allocation(reference.allocation, allocation)) {
            return &reference;
        }
    }
    return nullptr;
}

/**
 * Counts exact peer and ticket owners for one allocator slot.
 * @param record Owning activity context.
 * @param index Allocator slot index.
 * @param references Receives aggregate counts for diagnostics and reclaim checks.
 */
void collect_references(const ContextRecord& record,
                        std::size_t index,
                        std::array<std::uint16_t, kReferenceKindCount>& references) noexcept {
    references = {};
    const SlotRecord& slot = record.slots[index];
    for (const HostReferenceTicketRecord& ticket : record.hostTickets) {
        if (ticket.occupied && allocation_matches_slot(slot, index, ticket.allocation)) {
            ++references[reference_index(ticket.kind)];
        }
    }
    for (const PeerReplicaRecord& peer : record.peers) {
        if (!peer.occupied) {
            continue;
        }
        for (const PeerAllocationReferenceRecord& reference : peer.references) {
            if (reference.occupied && allocation_matches_slot(slot, index, reference.allocation)) {
                ++references[reference_index(ReferenceKind::peer)];
            }
        }
        for (const ContributionTicketRecord& ticket : peer.tickets) {
            if (ticket.occupied && allocation_matches_slot(slot, index, ticket.allocation)) {
                ++references[reference_index(contribution_reference_kind(ticket.kind))];
            }
        }
    }
}

/**
 * Checks whether every exact owner of one allocator identity has drained.
 * @param record Owning activity context.
 * @param index Allocator slot index.
 * @return True when no peer, ticket, dependency, or command retains it.
 */
[[nodiscard]] bool references_empty(const ContextRecord& record, std::size_t index) noexcept {
    std::array<std::uint16_t, kReferenceKindCount> references{};
    collect_references(record, index, references);
    for (const std::uint16_t count : references) {
        if (count != 0) {
            return false;
        }
    }
    return true;
}

/**
 * Makes a retired slot reusable when no old identity can still be observed.
 * @param record Owning activity context.
 * @param index Quarantined server slot index.
 * @return True when the slot became free.
 */
[[nodiscard]] bool reclaim(ContextRecord& record, std::size_t index) noexcept {
    SlotRecord& slot = record.slots[index];
    if (slot.lifecycle != Lifecycle::quarantined || slot.actorId == kAbsentActorId
        || slot.quarantineReason != QuarantineReason::retired || !references_empty(record, index)) {
        return false;
    }
    if (slot.incarnationUseCount >= kWireIncarnationCount) {
        slot.quarantineReason = QuarantineReason::incarnationExhausted;
        return false;
    }
    slot.actorId = kAbsentActorId;
    slot.incarnation = static_cast<std::uint8_t>((slot.incarnation + 1U) & kWireIncarnationMask);
    slot.lifecycle = Lifecycle::free;
    slot.quarantineReason = QuarantineReason::none;
    return true;
}

/**
 * Reclaims the slot named by a peer-owned identity after one exact owner is removed.
 * @param record Owning activity context.
 * @param allocation Allocation whose owner was removed.
 */
void reclaim_after_release(ContextRecord& record, const Allocation& allocation) noexcept {
    if (resolve_allocation(record, allocation) != nullptr) {
        static_cast<void>(reclaim(record, allocation.token.index));
    }
}

/**
 * Releases every exact allocation and ticket owned by one peer replica.
 * @param record Owning activity context.
 * @param peer Peer replica being replaced or retired.
 */
void clear_peer_ownership(ContextRecord& record, PeerReplicaRecord& peer) noexcept {
    for (ContributionTicketRecord& ticket : peer.tickets) {
        if (!ticket.occupied) {
            continue;
        }
        const Allocation allocation = ticket.allocation;
        SecureZeroMemory(&ticket, sizeof ticket);
        reclaim_after_release(record, allocation);
    }
    for (PeerAllocationReferenceRecord& reference : peer.references) {
        if (!reference.occupied) {
            continue;
        }
        const Allocation allocation = reference.allocation;
        SecureZeroMemory(&reference, sizeof reference);
        reclaim_after_release(record, allocation);
    }
}

/**
 * Initializes lease ownership and reset identity for a newly published context.
 * @param record Cleared fixed context slot.
 * @param context Checked activity snapshot.
 */
void initialize_context(ContextRecord& record, const ActivityReplicaContext& context) noexcept {
    record.context = context;
    record.reuseEpoch = kFirstReuseEpoch;
    for (std::size_t index = 0; index < record.slots.size(); ++index) {
        SlotRecord& slot = record.slots[index];
        if (mask_has(context.clientLeaseMask, index)) {
            slot.owner = LeaseOwner::client;
            slot.lifecycle = Lifecycle::absent;
        } else if (mask_has(context.serverReserveMask, index)) {
            slot.owner = LeaseOwner::server;
            slot.lifecycle = Lifecycle::free;
        } else {
            slot.owner = LeaseOwner::free;
            slot.lifecycle = Lifecycle::absent;
        }
    }
}

/**
 * Writes one allocation value from its current fixed slot.
 * @param context Owning context handle.
 * @param index Logical entity index.
 * @param slot Reserved slot state.
 * @param allocation Receives the exact identity.
 */
void copy_allocation(ContextHandle context,
                     std::size_t index,
                     const SlotRecord& slot,
                     Allocation& allocation) noexcept {
    allocation.context = context;
    allocation.token.index = static_cast<std::uint16_t>(index);
    allocation.token.incarnation = slot.incarnation;
    allocation.actorId = slot.actorId;
    allocation.generation = slot.allocationGeneration;
    allocation.sequence = slot.allocationSequence;
}

/** @param peer Peer record. @return Number of exact allocation references it owns. */
[[nodiscard]] std::size_t peer_reference_count(const PeerReplicaRecord& peer) noexcept {
    std::size_t count = 0;
    for (const PeerAllocationReferenceRecord& reference : peer.references) {
        count += reference.occupied ? 1U : 0U;
    }
    return count;
}

/** @param peer Peer record. @return Number of exact contribution tickets it owns. */
[[nodiscard]] std::size_t peer_ticket_count(const PeerReplicaRecord& peer) noexcept {
    std::size_t count = 0;
    for (const ContributionTicketRecord& ticket : peer.tickets) {
        count += ticket.occupied ? 1U : 0U;
    }
    return count;
}

} // namespace

/** Clears fixed storage and makes the dormant replica state available. */
void initialize() noexcept {
    AcquireSRWLockExclusive(&g_storage.lock);
    clear_storage();
    g_storage.ready = true;
    ReleaseSRWLockExclusive(&g_storage.lock);
}

/** Clears every context and invalidates all outstanding handles. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_storage.lock);
    clear_storage();
    ReleaseSRWLockExclusive(&g_storage.lock);
}

/** Publishes one checked activity and initializes its lease owners. */
bool publish_context(const ActivityReplicaContext& context, ContextHandle& handle) noexcept {
    handle = {};
    handle.slot = kInvalidContextSlot;
    if (!valid_role(context.role) || context.activitySessionId == kAbsentActivitySessionId
        || !masks_are_disjoint(context)) {
        return false;
    }

    AcquireSRWLockExclusive(&g_storage.lock);
    if (!g_storage.ready) {
        ReleaseSRWLockExclusive(&g_storage.lock);
        return false;
    }
    ContextRecord* target = nullptr;
    std::size_t targetIndex = kInvalidContextSlot;
    for (std::size_t index = 0; index < g_storage.contexts.size(); ++index) {
        ContextRecord& record = g_storage.contexts[index];
        if (record.occupied && record.context.role == context.role
            && record.context.activitySessionId == context.activitySessionId) {
            ReleaseSRWLockExclusive(&g_storage.lock);
            return false;
        }
        if (target == nullptr && !record.occupied) {
            target = &record;
            targetIndex = index;
        }
    }
    std::uint64_t generation = kAbsentHandleGeneration;
    if (target == nullptr || !claim_handle_generation(generation)) {
        ReleaseSRWLockExclusive(&g_storage.lock);
        return false;
    }

    SecureZeroMemory(target, sizeof *target);
    target->generation = generation;
    initialize_context(*target, context);
    target->occupied = true;
    handle = make_context_handle(*target, targetIndex);
    ReleaseSRWLockExclusive(&g_storage.lock);
    return true;
}

/** Finds an exact role/session pair without aliasing other roles. */
bool find_context(ReplicaRole role,
                  std::uint64_t activitySessionId,
                  ContextHandle& handle) noexcept {
    handle = {};
    handle.slot = kInvalidContextSlot;
    if (!valid_role(role) || activitySessionId == kAbsentActivitySessionId) {
        return false;
    }
    AcquireSRWLockShared(&g_storage.lock);
    if (g_storage.ready) {
        for (std::size_t index = 0; index < g_storage.contexts.size(); ++index) {
            const ContextRecord& record = g_storage.contexts[index];
            if (record.occupied && record.context.role == role
                && record.context.activitySessionId == activitySessionId) {
                handle = make_context_handle(record, index);
                ReleaseSRWLockShared(&g_storage.lock);
                return true;
            }
        }
    }
    ReleaseSRWLockShared(&g_storage.lock);
    return false;
}

/** Copies one activity context without leaking State-owned pointers. */
bool snapshot_context(ContextHandle handle, ActivityReplicaContext& context) noexcept {
    context = {};
    AcquireSRWLockShared(&g_storage.lock);
    const ContextRecord* record = resolve_context_const(handle);
    if (record != nullptr) {
        context = record->context;
    }
    ReleaseSRWLockShared(&g_storage.lock);
    return record != nullptr;
}

/** Copies the current activity-wide reuse epoch. */
bool snapshot_reuse_epoch(ContextHandle handle, std::uint64_t& reuseEpoch) noexcept {
    reuseEpoch = 0;
    AcquireSRWLockShared(&g_storage.lock);
    const ContextRecord* record = resolve_context_const(handle);
    if (record != nullptr) {
        reuseEpoch = record->reuseEpoch;
    }
    ReleaseSRWLockShared(&g_storage.lock);
    return record != nullptr;
}

/** Copies one allocator slot without exposing fixed State storage. */
bool snapshot_slot(ContextHandle handle, std::size_t index, SlotSnapshot& snapshot) noexcept {
    snapshot = {};
    AcquireSRWLockShared(&g_storage.lock);
    const ContextRecord* record = resolve_context_const(handle);
    if (record == nullptr || index >= record->slots.size()) {
        ReleaseSRWLockShared(&g_storage.lock);
        return false;
    }
    const SlotRecord& slot = record->slots[index];
    collect_references(*record, index, snapshot.references);
    snapshot.actorId = slot.actorId;
    snapshot.allocationGeneration = slot.allocationGeneration;
    snapshot.allocationSequence = slot.allocationSequence;
    snapshot.incarnation = slot.incarnation;
    snapshot.incarnationUseCount = slot.incarnationUseCount;
    snapshot.owner = slot.owner;
    snapshot.lifecycle = slot.lifecycle;
    snapshot.quarantineReason = slot.quarantineReason;
    ReleaseSRWLockShared(&g_storage.lock);
    return true;
}

/** Tears down one activity context and invalidates its allocations and peer replicas. */
bool reset_context(ContextHandle handle) noexcept {
    AcquireSRWLockExclusive(&g_storage.lock);
    ContextRecord* record = resolve_context(handle);
    if (record != nullptr) {
        SecureZeroMemory(record, sizeof *record);
    }
    ReleaseSRWLockExclusive(&g_storage.lock);
    return record != nullptr;
}

/**
 * Resets wire incarnation budgets after the whole activity has drained.
 * @param handle Exact activity context.
 * @return True when no actor owner, peer, or ticket remains.
 */
bool advance_reuse_epoch(ContextHandle handle) noexcept {
    AcquireSRWLockExclusive(&g_storage.lock);
    ContextRecord* record = resolve_context(handle);
    if (record == nullptr || record->reuseEpoch == (std::numeric_limits<std::uint64_t>::max)()) {
        ReleaseSRWLockExclusive(&g_storage.lock);
        return false;
    }
    for (const PeerReplicaRecord& peer : record->peers) {
        if (peer.occupied) {
            ReleaseSRWLockExclusive(&g_storage.lock);
            return false;
        }
    }
    for (const HostReferenceTicketRecord& ticket : record->hostTickets) {
        if (ticket.occupied) {
            ReleaseSRWLockExclusive(&g_storage.lock);
            return false;
        }
    }
    for (const SlotRecord& slot : record->slots) {
        if (slot.owner != LeaseOwner::server) {
            if (slot.actorId != kAbsentActorId || slot.lifecycle != Lifecycle::absent) {
                ReleaseSRWLockExclusive(&g_storage.lock);
                return false;
            }
            continue;
        }
        const bool free = slot.lifecycle == Lifecycle::free && slot.actorId == kAbsentActorId
                          && slot.quarantineReason == QuarantineReason::none;
        const bool quarantined =
            slot.lifecycle == Lifecycle::quarantined && slot.actorId != kAbsentActorId
            && (slot.quarantineReason == QuarantineReason::aborted
                || slot.quarantineReason == QuarantineReason::incarnationExhausted);
        if (!free && !quarantined) {
            ReleaseSRWLockExclusive(&g_storage.lock);
            return false;
        }
    }

    ++record->reuseEpoch;
    for (SlotRecord& slot : record->slots) {
        if (slot.owner != LeaseOwner::server) {
            continue;
        }
        slot.actorId = kAbsentActorId;
        slot.incarnation = 0;
        slot.incarnationUseCount = 0;
        slot.lifecycle = Lifecycle::free;
        slot.quarantineReason = QuarantineReason::none;
    }
    ReleaseSRWLockExclusive(&g_storage.lock);
    return true;
}

/** Reserves the first available server-owned index for one actor. */
bool reserve(ContextHandle handle, std::uint64_t actorId, Allocation& allocation) noexcept {
    allocation = {};
    allocation.context.slot = kInvalidContextSlot;
    if (actorId == kAbsentActorId) {
        return false;
    }
    AcquireSRWLockExclusive(&g_storage.lock);
    ContextRecord* record = resolve_context(handle);
    if (record == nullptr) {
        ReleaseSRWLockExclusive(&g_storage.lock);
        return false;
    }
    for (const SlotRecord& slot : record->slots) {
        if (slot.actorId == actorId) {
            ReleaseSRWLockExclusive(&g_storage.lock);
            return false;
        }
    }
    for (std::size_t index = 0; index < record->slots.size(); ++index) {
        SlotRecord& slot = record->slots[index];
        if (slot.owner != LeaseOwner::server || slot.lifecycle != Lifecycle::free
            || slot.actorId != kAbsentActorId || !references_empty(*record, index)
            || slot.incarnationUseCount >= kWireIncarnationCount
            || slot.allocationGeneration == (std::numeric_limits<std::uint64_t>::max)()) {
            continue;
        }
        slot.allocationSequence = next_allocation_sequence(slot.allocationSequence);
        ++slot.allocationGeneration;
        ++slot.incarnationUseCount;
        slot.actorId = actorId;
        slot.lifecycle = Lifecycle::reserved;
        slot.quarantineReason = QuarantineReason::none;
        copy_allocation(handle, index, slot, allocation);
        ReleaseSRWLockExclusive(&g_storage.lock);
        return true;
    }
    ReleaseSRWLockExclusive(&g_storage.lock);
    return false;
}

/**
 * Quarantines a reservation after preparation fails.
 * @param allocation Exact reserved allocation.
 * @return True when the allocation entered abort quarantine.
 */
bool abort_reservation(const Allocation& allocation) noexcept {
    AcquireSRWLockExclusive(&g_storage.lock);
    ContextRecord* record = nullptr;
    SlotRecord* slot = resolve_allocation(allocation, record);
    const bool valid = slot != nullptr && slot->lifecycle == Lifecycle::reserved;
    if (valid) {
        slot->lifecycle = Lifecycle::quarantined;
        slot->quarantineReason = QuarantineReason::aborted;
    }
    ReleaseSRWLockExclusive(&g_storage.lock);
    return valid;
}

/** Advances a reserved allocation to create-queued. */
bool queue_create(const Allocation& allocation) noexcept {
    AcquireSRWLockExclusive(&g_storage.lock);
    ContextRecord* record = nullptr;
    SlotRecord* slot = resolve_allocation(allocation, record);
    const bool valid = slot != nullptr && slot->lifecycle == Lifecycle::reserved;
    if (valid) {
        slot->lifecycle = Lifecycle::createQueued;
    }
    ReleaseSRWLockExclusive(&g_storage.lock);
    return valid;
}

/** Commits a create-queued allocation as live. */
bool commit_create(const Allocation& allocation) noexcept {
    AcquireSRWLockExclusive(&g_storage.lock);
    ContextRecord* record = nullptr;
    SlotRecord* slot = resolve_allocation(allocation, record);
    const bool valid = slot != nullptr && slot->lifecycle == Lifecycle::createQueued;
    if (valid) {
        slot->lifecycle = Lifecycle::live;
    }
    ReleaseSRWLockExclusive(&g_storage.lock);
    return valid;
}

/** Advances a created allocation to remove-queued. */
bool queue_remove(const Allocation& allocation) noexcept {
    AcquireSRWLockExclusive(&g_storage.lock);
    ContextRecord* record = nullptr;
    SlotRecord* slot = resolve_allocation(allocation, record);
    const bool valid =
        slot != nullptr
        && (slot->lifecycle == Lifecycle::createQueued || slot->lifecycle == Lifecycle::live);
    if (valid) {
        slot->lifecycle = Lifecycle::removeQueued;
    }
    ReleaseSRWLockExclusive(&g_storage.lock);
    return valid;
}

/**
 * Retires a remove-queued actor and reclaims it after every owner drains.
 * @param allocation Exact remove-queued allocation.
 * @return True when retirement was committed.
 */
bool settle_remove(const Allocation& allocation) noexcept {
    AcquireSRWLockExclusive(&g_storage.lock);
    ContextRecord* record = nullptr;
    SlotRecord* slot = resolve_allocation(allocation, record);
    const bool valid = slot != nullptr && slot->lifecycle == Lifecycle::removeQueued;
    if (valid) {
        slot->lifecycle = Lifecycle::quarantined;
        slot->quarantineReason = QuarantineReason::retired;
        static_cast<void>(reclaim(*record, allocation.token.index));
    }
    ReleaseSRWLockExclusive(&g_storage.lock);
    return valid;
}

/** Publishes one peer-owned view under an exact activity context. */
bool publish_peer_replica(ContextHandle context,
                          const ViewKey& view,
                          PeerReplicaHandle& replica) noexcept {
    replica = {};
    replica.context.slot = kInvalidContextSlot;
    replica.slot = kInvalidPeerReplicaSlot;
    if (!valid_view_key(view)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_storage.lock);
    ContextRecord* record = resolve_context(context);
    if (record == nullptr) {
        ReleaseSRWLockExclusive(&g_storage.lock);
        return false;
    }
    PeerReplicaRecord* target = nullptr;
    std::size_t targetIndex = kInvalidPeerReplicaSlot;
    for (std::size_t index = 0; index < record->peers.size(); ++index) {
        PeerReplicaRecord& peer = record->peers[index];
        if (peer.occupied && equal_view_owner(peer.view, view)) {
            ReleaseSRWLockExclusive(&g_storage.lock);
            return false;
        }
        if (target == nullptr && !peer.occupied) {
            target = &peer;
            targetIndex = index;
        }
    }
    std::uint64_t generation = kAbsentHandleGeneration;
    if (target == nullptr || !claim_handle_generation(generation)) {
        ReleaseSRWLockExclusive(&g_storage.lock);
        return false;
    }
    SecureZeroMemory(target, sizeof *target);
    target->view = view;
    target->generation = generation;
    target->occupied = true;
    replica = make_peer_handle(context, *target, targetIndex);
    ReleaseSRWLockExclusive(&g_storage.lock);
    return true;
}

/** Copies one peer replica without exposing State-owned pointers. */
bool snapshot_peer_replica(PeerReplicaHandle replica, PeerReplicaSnapshot& snapshot) noexcept {
    snapshot = {};
    AcquireSRWLockShared(&g_storage.lock);
    const ContextRecord* context = nullptr;
    const PeerReplicaRecord* peer = resolve_peer_const(replica, context);
    if (peer != nullptr) {
        snapshot.view = peer->view;
        snapshot.allocationReferenceCount = peer_reference_count(*peer);
        snapshot.contributionTicketCount = peer_ticket_count(*peer);
    }
    ReleaseSRWLockShared(&g_storage.lock);
    return peer != nullptr;
}

/**
 * Replaces one peer's view without changing its peer or group owner.
 * @param replica Exact old peer replica.
 * @param view New view for the same peer and group owner.
 * @param replacement Receives the new generation-checked replica.
 * @return True when the view was identical or replaced.
 */
bool replace_peer_view(PeerReplicaHandle replica,
                       const ViewKey& view,
                       PeerReplicaHandle& replacement) noexcept {
    replacement = {};
    replacement.context.slot = kInvalidContextSlot;
    replacement.slot = kInvalidPeerReplicaSlot;
    if (!valid_view_key(view)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_storage.lock);
    ContextRecord* record = nullptr;
    PeerReplicaRecord* peer = resolve_peer(replica, record);
    if (peer == nullptr || !equal_peer_key(peer->view.peer, view.peer)
        || peer->view.groupSessionId != view.groupSessionId) {
        ReleaseSRWLockExclusive(&g_storage.lock);
        return false;
    }
    if (equal_view_key(peer->view, view)) {
        replacement = replica;
        ReleaseSRWLockExclusive(&g_storage.lock);
        return true;
    }
    for (const PeerReplicaRecord& candidate : record->peers) {
        if (&candidate != peer && candidate.occupied && equal_view_owner(candidate.view, view)) {
            ReleaseSRWLockExclusive(&g_storage.lock);
            return false;
        }
    }
    std::uint64_t generation = kAbsentHandleGeneration;
    if (!claim_handle_generation(generation)) {
        ReleaseSRWLockExclusive(&g_storage.lock);
        return false;
    }

    clear_peer_ownership(*record, *peer);
    peer->view = view;
    peer->generation = generation;
    replacement = make_peer_handle(replica.context, *peer, replica.slot);
    ReleaseSRWLockExclusive(&g_storage.lock);
    return true;
}

/** Retires one peer replica and releases only its owned references and tickets. */
bool retire_peer_replica(PeerReplicaHandle replica) noexcept {
    AcquireSRWLockExclusive(&g_storage.lock);
    ContextRecord* record = nullptr;
    PeerReplicaRecord* peer = resolve_peer(replica, record);
    if (peer != nullptr) {
        clear_peer_ownership(*record, *peer);
        SecureZeroMemory(peer, sizeof *peer);
    }
    ReleaseSRWLockExclusive(&g_storage.lock);
    return peer != nullptr;
}

/** Retains one allocation in one peer's baseline or relevance state. */
bool retain_for_peer(PeerReplicaHandle replica, const Allocation& allocation) noexcept {
    AcquireSRWLockExclusive(&g_storage.lock);
    ContextRecord* record = nullptr;
    PeerReplicaRecord* peer = resolve_peer(replica, record);
    SlotRecord* slot = record == nullptr ? nullptr : resolve_allocation(*record, allocation);
    if (peer == nullptr || !equal_context_handle(replica.context, allocation.context)
        || slot == nullptr || slot->lifecycle == Lifecycle::quarantined
        || find_peer_reference(*peer, allocation) != nullptr) {
        ReleaseSRWLockExclusive(&g_storage.lock);
        return false;
    }
    for (PeerAllocationReferenceRecord& reference : peer->references) {
        if (!reference.occupied) {
            reference.allocation = allocation;
            reference.occupied = true;
            ReleaseSRWLockExclusive(&g_storage.lock);
            return true;
        }
    }
    ReleaseSRWLockExclusive(&g_storage.lock);
    return false;
}

/** Releases one allocation from one peer's baseline or relevance state. */
bool release_for_peer(PeerReplicaHandle replica, const Allocation& allocation) noexcept {
    AcquireSRWLockExclusive(&g_storage.lock);
    ContextRecord* record = nullptr;
    PeerReplicaRecord* peer = resolve_peer(replica, record);
    PeerAllocationReferenceRecord* reference =
        peer == nullptr ? nullptr : find_peer_reference(*peer, allocation);
    if (reference != nullptr) {
        SecureZeroMemory(reference, sizeof *reference);
        reclaim_after_release(*record, allocation);
    }
    ReleaseSRWLockExclusive(&g_storage.lock);
    return reference != nullptr;
}

/** Opens one exact packet or replay-horizon reference owned by a peer replica. */
bool open_contribution_ticket(PeerReplicaHandle replica,
                              const Allocation& allocation,
                              ContributionKind kind,
                              ContributionTicket& ticket) noexcept {
    ticket = {};
    ticket.replica.context.slot = kInvalidContextSlot;
    ticket.replica.slot = kInvalidPeerReplicaSlot;
    ticket.slot = kInvalidContributionTicketSlot;
    if (!valid_contribution_kind(kind)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_storage.lock);
    ContextRecord* record = nullptr;
    PeerReplicaRecord* peer = resolve_peer(replica, record);
    SlotRecord* slot = record == nullptr ? nullptr : resolve_allocation(*record, allocation);
    if (peer == nullptr || !equal_context_handle(replica.context, allocation.context)
        || slot == nullptr || slot->lifecycle == Lifecycle::quarantined
        || find_peer_reference(*peer, allocation) == nullptr) {
        ReleaseSRWLockExclusive(&g_storage.lock);
        return false;
    }
    ContributionTicketRecord* target = nullptr;
    std::size_t targetIndex = kInvalidContributionTicketSlot;
    for (std::size_t index = 0; index < peer->tickets.size(); ++index) {
        if (!peer->tickets[index].occupied) {
            target = &peer->tickets[index];
            targetIndex = index;
            break;
        }
    }
    std::uint64_t generation = kAbsentHandleGeneration;
    if (target == nullptr || !claim_handle_generation(generation)) {
        ReleaseSRWLockExclusive(&g_storage.lock);
        return false;
    }
    target->allocation = allocation;
    target->generation = generation;
    target->kind = kind;
    target->occupied = true;
    ticket.replica = replica;
    ticket.generation = generation;
    ticket.slot = targetIndex;
    ReleaseSRWLockExclusive(&g_storage.lock);
    return true;
}

/** Releases the exact reference owned by one contribution ticket. */
bool settle_contribution_ticket(ContributionTicket ticket) noexcept {
    AcquireSRWLockExclusive(&g_storage.lock);
    ContextRecord* record = nullptr;
    PeerReplicaRecord* peer = resolve_peer(ticket.replica, record);
    if (peer == nullptr || ticket.slot >= peer->tickets.size()
        || ticket.generation == kAbsentHandleGeneration) {
        ReleaseSRWLockExclusive(&g_storage.lock);
        return false;
    }
    ContributionTicketRecord& stored = peer->tickets[ticket.slot];
    if (!stored.occupied || stored.generation != ticket.generation) {
        ReleaseSRWLockExclusive(&g_storage.lock);
        return false;
    }
    const Allocation allocation = stored.allocation;
    SecureZeroMemory(&stored, sizeof stored);
    reclaim_after_release(*record, allocation);
    ReleaseSRWLockExclusive(&g_storage.lock);
    return true;
}

/**
 * Opens one exact dependency or command reference outside peer state.
 * @param allocation Exact active allocation.
 * @param kind Dependency or command owner kind.
 * @param ticket Receives the single-use generation-checked owner.
 * @return True when a free ticket was committed.
 */
bool open_host_reference_ticket(const Allocation& allocation,
                                ReferenceKind kind,
                                HostReferenceTicket& ticket) noexcept {
    ticket = {};
    ticket.context.slot = kInvalidContextSlot;
    ticket.slot = kInvalidHostReferenceTicketSlot;
    if (!valid_host_reference_kind(kind)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_storage.lock);
    ContextRecord* record = nullptr;
    SlotRecord* slot = resolve_allocation(allocation, record);
    if (slot == nullptr || slot->lifecycle == Lifecycle::quarantined) {
        ReleaseSRWLockExclusive(&g_storage.lock);
        return false;
    }
    HostReferenceTicketRecord* target = nullptr;
    std::size_t targetIndex = kInvalidHostReferenceTicketSlot;
    for (std::size_t index = 0; index < record->hostTickets.size(); ++index) {
        if (!record->hostTickets[index].occupied) {
            target = &record->hostTickets[index];
            targetIndex = index;
            break;
        }
    }
    std::uint64_t generation = kAbsentHandleGeneration;
    if (target == nullptr || !claim_handle_generation(generation)) {
        ReleaseSRWLockExclusive(&g_storage.lock);
        return false;
    }
    target->allocation = allocation;
    target->generation = generation;
    target->kind = kind;
    target->occupied = true;
    ticket.context = allocation.context;
    ticket.generation = generation;
    ticket.slot = targetIndex;
    ReleaseSRWLockExclusive(&g_storage.lock);
    return true;
}

/**
 * Releases one exact dependency or command reference.
 * @param ticket Exact single-use host-reference ticket.
 * @return True when the ticket was live and settled once.
 */
bool settle_host_reference_ticket(HostReferenceTicket ticket) noexcept {
    AcquireSRWLockExclusive(&g_storage.lock);
    ContextRecord* record = resolve_context(ticket.context);
    if (record == nullptr || ticket.slot >= record->hostTickets.size()
        || ticket.generation == kAbsentHandleGeneration) {
        ReleaseSRWLockExclusive(&g_storage.lock);
        return false;
    }
    HostReferenceTicketRecord& stored = record->hostTickets[ticket.slot];
    if (!stored.occupied || stored.generation != ticket.generation) {
        ReleaseSRWLockExclusive(&g_storage.lock);
        return false;
    }
    const Allocation allocation = stored.allocation;
    SecureZeroMemory(&stored, sizeof stored);
    reclaim_after_release(*record, allocation);
    ReleaseSRWLockExclusive(&g_storage.lock);
    return true;
}

} // namespace sunrise::state::gameplay::physics
