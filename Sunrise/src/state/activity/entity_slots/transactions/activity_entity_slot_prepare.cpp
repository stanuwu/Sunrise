#include <Windows.h>

#include <cstddef>
#include <cstdint>

#include "../../../account/account_context.h"
#include "../../../account/public_profiles.h"
#include "../../../runtime/storage/internal.h"
#include "../../member_context.h"
#include "../../member_selection.h"
#include "../../transactions/internal.h"
#include "../runtime.h"
#include "internal.h"
#include "member_join_authorization.h"

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
    mutation.shared = record.sharedMembers;
    if (requireJoined && record.sharedMembers) {
        const auto context = member_context();
        mutation.memberRow = member_row(record,
                                        context.sessionId == sessionId ? context.memberKey : 0,
                                        account_primary_soid(bound_account()));
        const auto* identity = member_identity(record, mutation.memberRow);
        if (!identity) {
            return nullptr;
        }
        mutation.identity = *identity;
        mutation.identityGuard = *identity;
    }
    mutation.prepared = true;
    return &record;
}

} // namespace

/** Prepares a join. It resets the lease, so it picks low-index slots outside the server reserve. */
bool prepare_join(std::uint64_t sessionId,
                  std::uint64_t memberKey,
                  std::size_t grantCount,
                  std::size_t serverReserveCount,
                  PendingMutation& mutation,
                  const membership::Identity* identity,
                  const SessionBinding* source) noexcept {
    mutation = {};
    if (sessionId == kAbsentSessionId || grantCount == 0 || grantCount > kSlotCount
        || serverReserveCount >= kSlotCount || grantCount > kSlotCount - serverReserveCount) {
        return false;
    }

    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    PendingMutation prepared{};
    const SessionRecord* record = prepare_base(state, sessionId, false, prepared);
    if (record != nullptr) {
        // A join takes the member key the request carries. Each region gets its own activity
        // client with its own key, so keeping the previous held set would leave it ungranted.
        prepared.serverMask = transactions::reserve_high(serverReserveCount);
        prepared.mask = transactions::select_free(prepared.serverMask, grantCount);
        if (identity) {
            const auto account = bound_account();
            if (identity->accountSoid != account_primary_soid(account)
                || identity->opaqueSoid != account::profiles::selected_character(account)) {
                ReleaseSRWLockShared(&runtime::storage::g_stateLock);
                return false;
            }
            prepared.shared = true;
            prepared.identity = *identity;
            prepared.identityGuard = *identity;
            prepared.profileGeneration = account::profiles::generation(account);
            if (source) {
                const auto* sourceRecord =
                    transactions::authorized_source(state, *source, *identity);
                if (!sourceRecord || source->sessionId == sessionId) {
                    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
                    return false;
                }
                prepared.sourceBinding = *source;
                prepared.expectedSourceRevision = sourceRecord->recordRevision;
                prepared.sourceAuthorized = true;
            }
            prepared.memberRow = join_member_row(*record, *identity, prepared.sourceAuthorized);
            auto leases = record->memberLeases;
            if (const auto* previous = member_identity(*record, prepared.memberRow);
                previous && previous->memberKey != identity->memberKey) {
                prepared.replacesMemberKey = previous->memberKey;
                depart_member_lease(leases, prepared.memberRow);
            }
            prepared.memberLease =
                plan_member_lease(leases, prepared.memberRow, prepared.serverMask, grantCount);
            if (!prepared.memberLease.valid || identity->memberKey != memberKey
                || (record->joined && record->serverEntitySlots != prepared.serverMask)
                || !transactions::join_roster(*record,
                                              *identity,
                                              prepared.memberRow,
                                              prepared.peerReservationsAfter,
                                              prepared.replacesMemberKey)) {
                ReleaseSRWLockShared(&runtime::storage::g_stateLock);
                return false;
            }
            prepared.mask = prepared.memberLease.mask;
        } else if (record->sharedMembers) {
            ReleaseSRWLockShared(&runtime::storage::g_stateLock);
            return false;
        }
        prepared.memberKey = memberKey;
        prepared.expectedMemberKey = memberKey;
        prepared.requestedCount = grantCount;
        prepared.serverReserveCount = serverReserveCount;
        prepared.kind = MutationKind::join;
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
        // A later grant may never reach into the server complement.
        prepared.mask = transactions::select_free(
            transactions::unite(record->heldEntitySlots, record->serverEntitySlots),
            requestedCount);
        if (prepared.shared) {
            auto unavailable = record->memberLeases.held[prepared.memberRow];
            const auto owned = member_lease_block(record->memberLeases.blocks[prepared.memberRow],
                                                  record->memberLeases.blockWidth);
            for (std::size_t i = 0; i < unavailable.size(); ++i) {
                unavailable[i] |= ~owned[i];
            }
            prepared.mask = transactions::select_free(unavailable, requestedCount);
        }
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
        if (prepared.shared) {
            prepared.mask =
                transactions::intersect(record->memberLeases.held[prepared.memberRow], returned);
        }
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
