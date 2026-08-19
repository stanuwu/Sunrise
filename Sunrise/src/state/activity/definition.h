#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "bubble_authority/definition.h"
#include "defaults/definition.h"
#include "destination/definition.h"
#include "entity_slots/definition.h"
#include "forced/definition.h"
#include "membership/definition.h"
#include "receipts/definition.h"

namespace sunrise::state::activity {

/**
 * Records bound process-local session lookup with no heap storage. It must hold every live
 * session at once: the private one plus one activity host per region. A full table evicts the
 * oldest record, which can be the private one, so the margin is deliberate.
 */
inline constexpr std::size_t kSessionCapacity = 16;
/** Zero is reserved as the absent activity-session id. */
inline constexpr std::uint64_t kAbsentSessionId = 0;
/** An unjoined record keeps its member-key storage cleared. */
inline constexpr std::uint64_t kClearedMemberKey = 0;
/** Runtime activity-session ids begin at 1 because 0 means absent. */
inline constexpr std::uint64_t kFirstSessionId = 1;
/** A cleared revision cannot validate a prepared allocation. */
inline constexpr std::uint64_t kInvalidRevision = 0;
/** State revisions begin at 1 so cleared storage rejects pending allocations. */
inline constexpr std::uint64_t kInitialStateRevision = 1;
/** Allocator revisions begin at 1 so cleared storage rejects pending allocations. */
inline constexpr std::uint64_t kInitialAllocatorRevision = 1;
/** The session table size is also its invalid-slot sentinel. */
inline constexpr std::size_t kInvalidSessionSlot = kSessionCapacity;
/** The largest unsigned id commits once and then exhausts the allocator. */
inline constexpr std::uint64_t kMaximumSessionId = (std::numeric_limits<std::uint64_t>::max)();
/** Revisions never wrap because stale transactions could otherwise become valid again. */
inline constexpr std::uint64_t kMaximumRevision = (std::numeric_limits<std::uint64_t>::max)();

/** Immutable identity of one committed activity-session record generation. */
struct SessionBinding {
    /** Exact destination committed with the session record. */
    destination::DestinationSelection destination{};
    std::uint64_t sessionId{};
    /** Creation revision distinguishes a replacement that reuses the same session id. */
    std::uint64_t createdRevision{};
};

/** One committed activity-session id and its lifecycle revisions. */
struct SessionRecord {
    /** Scalar destination committed in the same transaction as this session. */
    destination::DestinationSelection destination{};
    /** Slots granted to this session and not yet returned by its client. */
    entity_slots::LeaseMask heldEntitySlots{};
    /**
     * Slots reserved for server-authored entities. Always disjoint from the held mask, and
     * never granted to a client. A released client slot returns to the free set, not here.
     */
    entity_slots::LeaseMask serverEntitySlots{};
    /** Membership data is valid only after this session binds its client key. */
    membership::MembershipState membership{};
    /** Bubble grant tokens reset with the bounded activity session record. */
    bubble_authority::AuthorityState bubbleAuthority{};
    std::uint64_t sessionId{};
    /** Join binds this key before any later client identity update can be accepted. */
    std::uint64_t memberKey{};
    std::uint64_t createdRevision{};
    /** Last State revision that changed this record. */
    std::uint64_t recordRevision{};
    /** Last successful join revision, or the invalid revision before any join. */
    std::uint64_t joinedRevision{};
    /** Live consumers retaining this exact record generation against release or eviction. */
    std::uint32_t bindingRetainCount{};
    bool occupied{};
    bool joined{};
};

/** Read-only allocation plan validated again under the State write lock. */
struct PendingAllocation {
    /** Validated destination captured before the allocation acquires its write lock. */
    destination::DestinationSelection destination{};
    std::uint64_t sessionId{};
    /** Account half the published soid carries, captured before the State lock is taken. */
    std::uint64_t soidBase{};
    std::uint64_t expectedStateRevision{};
    std::uint64_t expectedAllocatorRevision{};
    std::uint64_t expectedNextSessionId{};
    std::size_t targetSlot{kInvalidSessionSlot};
    /**
     * Set when this plan re-creates an id the allocator has already passed.
     * Such a plan takes the id it was given instead of the next one, and leaves the allocator
     * where it is, because the counter it fills was spent long ago.
     */
    bool recreated{};
    bool prepared{};
};

/** Process-local activity-session records and their monotonic allocator. */
struct ActivityState {
    std::array<SessionRecord, kSessionCapacity> sessions{};
    /** Immutable authored or configured fallback published during State initialization. */
    defaults::ActivityDefaults defaults{};
    /** Operator-chosen destination that replaces the client's own. Never saved. */
    forced::ForcedDestination forced{};
    /** One arrival row per activity message type, so no routed message is silently dropped. */
    receipts::ReceiptRegistry receipts{};
    std::uint64_t stateRevision{kInitialStateRevision};
    std::uint64_t nextSessionId{kFirstSessionId};
    std::uint64_t allocatorRevision{kInitialAllocatorRevision};
    bool allocatorExhausted{};
};

} // namespace sunrise::state::activity
