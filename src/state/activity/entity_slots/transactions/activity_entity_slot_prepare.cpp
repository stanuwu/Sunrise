#include <Windows.h>

#include <cstddef>
#include <cstdint>

#include "../../../runtime/storage/internal.h"
#include "../../transactions/internal.h"
#include "../runtime.h"
#include "internal.h"

namespace sunrise::state::activity::entity_slots {
namespace {

/**
 * Captures the shared session data while the root State read lock is held.
 * @param state Activity State, guarded by the root shared lock.
 * @param sessionId Existing nonzero activity session id.
 * @param requireJoined True when the record must already have a committed join.
 * @param mutation Cleared, then receives the captured session data.
 * @return Matching record when its revision can still advance, otherwise null.
 */
[[nodiscard]] const SessionRecord* prepare_base(const ActivityState& state,
                                                std::uint64_t sessionId,
                                                bool requireJoined,
                                                PendingMutation& mutation) noexcept {
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target == kInvalidSessionSlot || state.stateRevision == kMaximumRevision) {
        return nullptr;
    }
    const SessionRecord& record = state.sessions[target];
    if (record.recordRevision == kInvalidRevision || (requireJoined && !record.joined)) {
        return nullptr;
    }
    mutation.sessionId = sessionId;
    mutation.expectedStateRevision = state.stateRevision;
    mutation.expectedRecordRevision = record.recordRevision;
    mutation.targetSlot = target;
    mutation.prepared = true;
    return &record;
}

} // namespace

/** Prepares a join. Picks only low-index slots the session does not already hold. */
bool prepare_join(std::uint64_t sessionId,
                  std::uint64_t memberKey,
                  std::size_t grantCount,
                  PendingMutation& mutation) noexcept {
    mutation = {};
    if (sessionId == kAbsentSessionId || grantCount == 0 || grantCount > kSlotCount) {
        return false;
    }

    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    PendingMutation prepared{};
    const SessionRecord* record = prepare_base(state, sessionId, false, prepared);
    if (record != nullptr && (!record->joined || record->memberKey == memberKey)) {
        prepared.mask = transactions::select_free(record->heldEntitySlots, grantCount);
        prepared.memberKey = memberKey;
        prepared.expectedMemberKey = memberKey;
        prepared.requestedCount = grantCount;
        prepared.kind = MutationKind::join;
    } else {
        record = nullptr;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    if (record == nullptr) {
        return false;
    }
    mutation = prepared;
    return true;
}

/** Prepares up to the requested number of free low-index slots. */
bool prepare_grant(std::uint64_t sessionId,
                   std::size_t requestedCount,
                   PendingMutation& mutation) noexcept {
    mutation = {};
    if (sessionId == kAbsentSessionId || requestedCount == 0) {
        return false;
    }

    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    PendingMutation prepared{};
    const SessionRecord* record = prepare_base(state, sessionId, true, prepared);
    if (record != nullptr) {
        prepared.mask = transactions::select_free(record->heldEntitySlots, requestedCount);
        prepared.requestedCount = requestedCount;
        prepared.kind = MutationKind::grant;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    if (record == nullptr) {
        return false;
    }
    mutation = prepared;
    return true;
}

/** Prepares the overlap of returned and currently held slots. */
bool prepare_release(std::uint64_t sessionId,
                     const LeaseMask& returned,
                     PendingMutation& mutation) noexcept {
    mutation = {};
    if (sessionId == kAbsentSessionId) {
        return false;
    }

    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    PendingMutation prepared{};
    const SessionRecord* record = prepare_base(state, sessionId, true, prepared);
    if (record != nullptr) {
        prepared.mask = transactions::intersect(record->heldEntitySlots, returned);
        prepared.returnedMask = returned;
        prepared.kind = MutationKind::release;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    if (record == nullptr) {
        return false;
    }
    mutation = prepared;
    return true;
}

} // namespace sunrise::state::activity::entity_slots
