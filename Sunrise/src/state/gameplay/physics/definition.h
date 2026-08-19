#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../activity/entity_slots/definition.h"

namespace sunrise::state::gameplay::physics {

/** The fixed activity table holds every overlapping private and public replica context. */
inline constexpr std::size_t kReplicaContextCapacity = 16;
/** A wire entity index is 13 bits, so one activity owns exactly 8,192 slots. */
inline constexpr std::size_t kEntitySlotCount = 8'192;
/** The first live envelope supports four protected peers in one activity. */
inline constexpr std::size_t kPeerReplicaCapacity = 4;
/** One peer may retain every server actor allowed by the first live envelope. */
inline constexpr std::size_t kPeerAllocationReferenceCapacity = 256;
/** One peer may own at most 64 in-flight external contribution outcomes. */
inline constexpr std::size_t kContributionTicketCapacity = 64;
/** The first live envelope budgets 1,024 dependency and command owners per activity. */
inline constexpr std::size_t kHostReferenceTicketCapacity = 1'024;
/** A wire entity incarnation is 4 bits and has 16 distinct values in one reuse epoch. */
inline constexpr std::uint8_t kWireIncarnationCount = 16;
/** A zero activity id never identifies a replica context. */
inline constexpr std::uint64_t kAbsentActivitySessionId = 0;
/** A zero process-local generation makes a cleared handle fail closed. */
inline constexpr std::uint64_t kAbsentHandleGeneration = 0;
/** A zero actor id means that no actor owns a reserved slot. */
inline constexpr std::uint64_t kAbsentActorId = 0;
/** A zero peer member id cannot identify an authenticated peer. */
inline constexpr std::uint64_t kAbsentMemberId = 0;
/** A zero group session id cannot identify a bound peer view. */
inline constexpr std::uint64_t kAbsentGroupSessionId = 0;
/** A zero view epoch cannot distinguish a live view from cleared storage. */
inline constexpr std::uint64_t kAbsentViewEpoch = 0;
/** A zero association epoch cannot distinguish a live association from cleared storage. */
inline constexpr std::uint64_t kAbsentAssociationEpoch = 0;
/** A zero channel epoch cannot distinguish a live channel from cleared storage. */
inline constexpr std::uint64_t kAbsentChannelEpoch = 0;
/** A new activity begins in reuse epoch one after its lease transaction commits. */
inline constexpr std::uint64_t kFirstReuseEpoch = 1;
/** The context table size is also its invalid slot sentinel. */
inline constexpr std::size_t kInvalidContextSlot = kReplicaContextCapacity;
/** The peer table size is also its invalid slot sentinel. */
inline constexpr std::size_t kInvalidPeerReplicaSlot = kPeerReplicaCapacity;
/** The ticket table size is also its invalid slot sentinel. */
inline constexpr std::size_t kInvalidContributionTicketSlot = kContributionTicketCapacity;
/** The host-reference table size is also its invalid slot sentinel. */
inline constexpr std::size_t kInvalidHostReferenceTicketSlot = kHostReferenceTicketCapacity;

static_assert(kEntitySlotCount == activity::entity_slots::kSlotCount);

/** The activity role is part of replica identity, even when numeric session ids match. */
enum class ReplicaRole : std::uint8_t {
    absent,
    privateActivity,
    currentActivity,
    publicTarget,
};

/** One entity index belongs to a client, the server reserve, or neither lease. */
enum class LeaseOwner : std::uint8_t {
    free,
    client,
    server,
};

/** Server entity lifecycle. Only server-reserved slots may leave absent. */
enum class Lifecycle : std::uint8_t {
    absent,
    reserved,
    createQueued,
    live,
    removeQueued,
    quarantined,
    free,
};

/** Quarantine reason controls whether draining references is enough to reclaim a slot. */
enum class QuarantineReason : std::uint8_t {
    none,
    retired,
    aborted,
    incarnationExhausted,
};

/** Aggregate counters mirror exact owners and protect an allocation from reuse. */
enum class ReferenceKind : std::uint8_t {
    peer,
    packet,
    replay,
    dependency,
    command,
    count,
};

/** A contribution ticket owns one packet or replay-horizon reference. */
enum class ContributionKind : std::uint8_t {
    packet,
    replay,
};

/** Reference counters are fixed because replica state may not allocate after startup. */
inline constexpr std::size_t kReferenceKindCount = static_cast<std::size_t>(ReferenceKind::count);

/** The exact 13-bit index and 4-bit incarnation written by a future entity codec. */
struct WireEntityToken final {
    std::uint16_t index{};
    std::uint8_t incarnation{};
};

/** A generation-checked handle to one fixed activity replica context. */
struct ContextHandle final {
    std::uint64_t generation{};
    std::size_t slot{kInvalidContextSlot};
};

/** Authenticated peer and carrier identity for one process-local channel generation. */
struct PeerKey final {
    std::uint64_t authenticatedMemberId{};
    std::uint64_t associationEpoch{};
    std::uint64_t remoteConnectionSequence{};
    std::uint64_t localConnectionSequence{};
    std::uint64_t channelEpoch{};
};

/** One peer-owned group view. It is never stored on the activity replica context. */
struct ViewKey final {
    PeerKey peer{};
    std::uint64_t groupSessionId{};
    std::uint64_t viewEpoch{};
};

/** A generation-checked handle to one peer replica owned by an activity context. */
struct PeerReplicaHandle final {
    ContextHandle context{};
    std::uint64_t generation{};
    std::size_t slot{kInvalidPeerReplicaSlot};
};

/** An exact handle to one peer-owned packet or replay reference. */
struct ContributionTicket final {
    PeerReplicaHandle replica{};
    std::uint64_t generation{};
    std::size_t slot{kInvalidContributionTicketSlot};
};

/** An exact generation-checked dependency or command reference. */
struct HostReferenceTicket final {
    ContextHandle context{};
    std::uint64_t generation{};
    std::size_t slot{kInvalidHostReferenceTicketSlot};
};

/**
 * Activity identity and the two masks committed by its lease transaction.
 * Views remain per-peer state and cannot be reset through this snapshot.
 */
struct ActivityReplicaContext final {
    activity::entity_slots::LeaseMask clientLeaseMask{};
    activity::entity_slots::LeaseMask serverReserveMask{};
    std::array<std::uint64_t, 2> patchEpoch{};
    std::uint64_t activitySessionId{};
    std::uint64_t bapConnectionId{};
    std::uint64_t rosterRevision{};
    std::uint64_t bubbleAuthorityRevision{};
    std::uint32_t currentBubble{};
    std::uint8_t reconciliationGeneration{};
    ReplicaRole role{ReplicaRole::absent};
};

/** An internal allocation identity that cannot alias the same wire token after reuse. */
struct Allocation final {
    ContextHandle context{};
    WireEntityToken token{};
    std::uint64_t actorId{};
    std::uint64_t generation{};
    std::uint8_t sequence{};
};

/** A bounded copy of one allocator slot for diagnostics and offline tests. */
struct SlotSnapshot final {
    std::array<std::uint16_t, kReferenceKindCount> references{};
    std::uint64_t actorId{};
    std::uint64_t allocationGeneration{};
    std::uint8_t allocationSequence{};
    std::uint8_t incarnation{};
    std::uint8_t incarnationUseCount{};
    LeaseOwner owner{LeaseOwner::free};
    Lifecycle lifecycle{Lifecycle::absent};
    QuarantineReason quarantineReason{QuarantineReason::none};
};

/** A bounded copy of one peer-owned view and its exact owner counts. */
struct PeerReplicaSnapshot final {
    ViewKey view{};
    std::size_t allocationReferenceCount{};
    std::size_t contributionTicketCount{};
};

} // namespace sunrise::state::gameplay::physics
