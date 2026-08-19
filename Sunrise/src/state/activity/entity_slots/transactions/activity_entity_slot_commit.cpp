#include <Windows.h>

#include <cstddef>

#include "../../../runtime/storage/internal.h"
#include "../../transactions/internal.h"
#include "../runtime.h"
#include "internal.h"

namespace sunrise::state::activity::entity_slots {
namespace {

/**
 * Applies one picked mask to its record. A join replaces the lease and the member key. A grant
 * adds to the lease and a release subtracts.
 * @param record Session record, held under the root write lock.
 * @param prepared Plan whose kind and mask already passed the checks.
 * @return True when the picked bits obey the operation's ownership rule.
 */
[[nodiscard]] bool apply(SessionRecord& record, const PendingMutation& prepared) noexcept {
    if (prepared.kind == MutationKind::join) {
        for (std::size_t index = 0; index < prepared.mask.size(); ++index) {
            if ((prepared.serverMask[index] & prepared.mask[index]) != std::byte{}) {
                return false;
            }
        }
        // A second region's activity client grants from a clear set, so it never waits for the
        // first one's slots to come back.
        record.heldEntitySlots = prepared.mask;
        // Both ownership masks become visible under the one revision this commit takes.
        record.serverEntitySlots = prepared.serverMask;
        record.memberKey = prepared.memberKey;
        // A new ActivityClient starts with no membership mirror or bubble grant.
        record.membership = {};
        record.bubbleAuthority = {};
        record.joined = true;
        return true;
    }
    if (!record.joined) {
        return false;
    }
    if (prepared.kind == MutationKind::grant) {
        for (std::size_t index = 0; index < prepared.mask.size(); ++index) {
            const bool overlapsClient =
                (record.heldEntitySlots[index] & prepared.mask[index]) != std::byte{};
            const bool overlapsServer =
                (record.serverEntitySlots[index] & prepared.mask[index]) != std::byte{};
            if (overlapsClient || overlapsServer) {
                return false;
            }
        }
        for (std::size_t index = 0; index < prepared.mask.size(); ++index) {
            record.heldEntitySlots[index] |= prepared.mask[index];
        }
        return true;
    }
    if (prepared.kind != MutationKind::release
        || transactions::exceeds(prepared.mask, record.heldEntitySlots)) {
        return false;
    }
    for (std::size_t index = 0; index < prepared.mask.size(); ++index) {
        record.heldEntitySlots[index] &= ~prepared.mask[index];
    }
    return true;
}

} // namespace

/** Commits one join, grant, or release when its captured revisions still match. */
bool commit(PendingMutation& mutation) noexcept {
    // Take the plan first so no mutation can replay, pass or fail.
    const PendingMutation prepared = mutation;
    mutation = {};
    if (!prepared.prepared || prepared.kind == MutationKind::none
        || prepared.sessionId == kAbsentSessionId
        || prepared.expectedStateRevision == kInvalidRevision
        || prepared.expectedRecordRevision == kInvalidRevision
        || prepared.targetSlot >= kSessionCapacity) {
        return false;
    }
    const bool countMutation =
        prepared.kind == MutationKind::join || prepared.kind == MutationKind::grant;
    if ((countMutation && !transactions::empty(prepared.returnedMask))
        || (prepared.kind == MutationKind::release && prepared.requestedCount != 0)
        || (prepared.kind == MutationKind::join && prepared.memberKey != prepared.expectedMemberKey)
        || (prepared.kind != MutationKind::join
            && (prepared.memberKey != kClearedMemberKey
                || prepared.expectedMemberKey != kClearedMemberKey
                || !transactions::empty(prepared.serverMask)
                || prepared.serverReserveCount != 0))) {
        return false;
    }

    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    SessionRecord& record = state.sessions[prepared.targetSlot];
    if (state.stateRevision == kMaximumRevision
        || state.stateRevision != prepared.expectedStateRevision || !record.occupied
        || record.sessionId != prepared.sessionId
        || record.recordRevision != prepared.expectedRecordRevision) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }

    LeaseMask expected{};
    if (prepared.kind == MutationKind::join) {
        const bool reserveFits =
            prepared.serverReserveCount < kSlotCount
            && prepared.requestedCount <= kSlotCount - prepared.serverReserveCount;
        if (prepared.requestedCount == 0 || !reserveFits) {
            ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
            return false;
        }
        // A record that has never joined must carry no key. A joined one may name any key,
        // because each region's activity client brings its own.
        if (!record.joined && record.memberKey != kClearedMemberKey) {
            ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
            return false;
        }
        const LeaseMask reserved = transactions::reserve_high(prepared.serverReserveCount);
        if (!transactions::equal(prepared.serverMask, reserved)) {
            ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
            return false;
        }
        // The held set is left out, because the join clears it.
        expected = transactions::select_free(reserved, prepared.requestedCount);
    } else if (prepared.kind == MutationKind::grant) {
        if (prepared.requestedCount == 0 || !record.joined) {
            ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
            return false;
        }
        expected = transactions::select_free(
            transactions::unite(record.heldEntitySlots, record.serverEntitySlots),
            prepared.requestedCount);
    } else if (prepared.kind == MutationKind::release) {
        if (!record.joined) {
            ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
            return false;
        }
        expected = transactions::intersect(record.heldEntitySlots, prepared.returnedMask);
    } else {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    if (!transactions::equal(prepared.mask, expected)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }

    const bool wasJoined = record.joined;
    const bool changesState =
        (prepared.kind == MutationKind::join && !wasJoined) || !transactions::empty(prepared.mask);
    if (!apply(record, prepared)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    if (changesState) {
        ++state.stateRevision;
        record.recordRevision = state.stateRevision;
        if (prepared.kind == MutationKind::join) {
            record.joinedRevision = state.stateRevision;
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return true;
}

/** Reports how many slots one session currently leases. */
bool lease_counts(std::uint64_t sessionId, std::size_t& held, std::size_t& reserved) noexcept {
    held = 0;
    reserved = 0;
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    const bool found = target != kInvalidSessionSlot && state.sessions[target].joined;
    if (found) {
        held = slot_count(state.sessions[target].heldEntitySlots);
        reserved = slot_count(state.sessions[target].serverEntitySlots);
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return found;
}

/** Copies both lease masks one session currently holds. */
bool lease_masks(std::uint64_t sessionId, LeaseMask& held, LeaseMask& reserved) noexcept {
    held = {};
    reserved = {};
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    const bool found = target != kInvalidSessionSlot && state.sessions[target].joined;
    if (found) {
        held = state.sessions[target].heldEntitySlots;
        reserved = state.sessions[target].serverEntitySlots;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return found;
}

} // namespace sunrise::state::activity::entity_slots
