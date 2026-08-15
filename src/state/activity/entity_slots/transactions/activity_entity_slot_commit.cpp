#include <Windows.h>

#include <cstddef>

#include "../../../runtime/storage/internal.h"
#include "../runtime.h"
#include "internal.h"

namespace sunrise::state::activity::entity_slots {
namespace {

/**
 * Applies one picked mask to its record. The caller already matched the member key
 * under the same lock.
 * @param record Session record, held under the root write lock.
 * @param prepared Plan whose kind and mask already passed the checks.
 * @return True when the picked bits obey the operation's ownership rule.
 */
[[nodiscard]] bool apply(SessionRecord& record, const PendingMutation& prepared) noexcept {
    if (prepared.kind == MutationKind::join) {
        for (std::size_t index = 0; index < prepared.mask.size(); ++index) {
            if ((record.heldEntitySlots[index] & prepared.mask[index]) != std::byte{}) {
                return false;
            }
        }
        for (std::size_t index = 0; index < prepared.mask.size(); ++index) {
            record.heldEntitySlots[index] |= prepared.mask[index];
        }
        if (!record.joined) {
            record.memberKey = prepared.memberKey;
        }
        record.joined = true;
        return true;
    }
    if (!record.joined) {
        return false;
    }
    if (prepared.kind == MutationKind::grant) {
        for (std::size_t index = 0; index < prepared.mask.size(); ++index) {
            if ((record.heldEntitySlots[index] & prepared.mask[index]) != std::byte{}) {
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
                || prepared.expectedMemberKey != kClearedMemberKey))) {
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
        if (prepared.requestedCount == 0 || prepared.requestedCount > kSlotCount) {
            ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
            return false;
        }
        if ((record.joined && record.memberKey != prepared.memberKey)
            || (!record.joined && record.memberKey != kClearedMemberKey)) {
            ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
            return false;
        }
        expected = transactions::select_free(record.heldEntitySlots, prepared.requestedCount);
    } else if (prepared.kind == MutationKind::grant) {
        if (prepared.requestedCount == 0 || !record.joined) {
            ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
            return false;
        }
        expected = transactions::select_free(record.heldEntitySlots, prepared.requestedCount);
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

} // namespace sunrise::state::activity::entity_slots
