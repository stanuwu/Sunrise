#include <Windows.h>

#include <cstddef>

#include "../../../account/account_context.h"
#include "../../../account/public_profiles.h"
#include "../../../runtime/storage/internal.h"
#include "../../member_mutation.h"
#include "../../member_selection.h"
#include "../../transactions/internal.h"
#include "../runtime.h"
#include "internal.h"
#include "member_join_authorization.h"

namespace sunrise::state::activity::entity_slots {
namespace {

/** Re-derives a shared member plan under the record lock before changing any owned state. */
bool apply_shared(ActivityState& state,
                  SessionRecord& record,
                  const PendingMutation& plan) noexcept {
    if (plan.memberRow >= kMemberLeaseRowCount || plan.identity != plan.identityGuard
        || plan.identity.accountSoid != account_primary_soid(bound_account())) {
        return false;
    }
    LeaseMask expected{};
    if (plan.kind == MutationKind::join) {
        reservations::Roster expectedRoster{};
        const auto* previous = member_identity(record, plan.memberRow);
        const auto previousKey =
            previous && previous->memberKey != plan.identity.memberKey ? previous->memberKey : 0;
        if (previousKey != plan.replacesMemberKey) {
            return false;
        }
        const auto* source =
            plan.sourceAuthorized
                ? transactions::authorized_source(state, plan.sourceBinding, plan.identity)
                : nullptr;
        if ((plan.sourceAuthorized
             && (!source || source->recordRevision != plan.expectedSourceRevision
                 || source->sessionId == record.sessionId))
            || (!plan.sourceAuthorized
                && (plan.expectedSourceRevision || plan.sourceBinding.sessionId))) {
            return false;
        }
        if (plan.identity.memberKey != plan.memberKey || plan.requestedCount == 0
            || plan.serverReserveCount >= kSlotCount
            || plan.requestedCount > kSlotCount - plan.serverReserveCount
            || plan.serverMask != transactions::reserve_high(plan.serverReserveCount)
            || (record.joined && record.serverEntitySlots != plan.serverMask)
            || join_member_row(record, plan.identity, plan.sourceAuthorized) != plan.memberRow
            || plan.identity.opaqueSoid != account::profiles::selected_character(bound_account())
            || plan.profileGeneration != account::profiles::generation(bound_account())
            || !transactions::join_roster(
                record, plan.identity, plan.memberRow, expectedRoster, previousKey)
            || expectedRoster != plan.peerReservationsAfter
            || !can_republish_members(record, plan.memberRow)) {
            return false;
        }
        auto leases = record.memberLeases;
        if (previousKey) {
            depart_member_lease(leases, plan.memberRow);
        }
        const auto derived =
            plan_member_lease(leases, plan.memberRow, plan.serverMask, plan.requestedCount);
        if (!derived.valid || derived != plan.memberLease || plan.mask != derived.mask
            || plan.replicationSequence != leases.replicationSequence) {
            return false;
        }
        const bool alreadyJoined = member_joined(record, plan.memberRow) && !previousKey;
        record.sharedMembers = true;
        record.serverEntitySlots = plan.serverMask;
        record.peerReservations = expectedRoster;
        record.memberLeases = leases;
        assign_member_lease(record.memberLeases, plan.memberRow, derived);
        if (!alreadyJoined) {
            if (plan.memberRow == 0) {
                record.memberKey = plan.memberKey;
                record.primaryIdentity = plan.identity;
                record.membership = {};
                record.joined = true;
            } else {
                auto& member = record.coMembers[plan.memberRow - 1];
                member = {};
                member.identity = plan.identity;
                member.joined = true;
            }
            auto& member = *member_state(record, plan.memberRow);
            member.epoch = membership::session_epoch(record.createdRevision);
            member.identity = plan.identity;
            member.hasIdentity = true;
            member.revision = membership::kInitialRevision;
            member.hasTransitionToken = true;
            member.transitionToken = membership::kInitialTransitionToken;
            republish_members(record, plan.memberRow);
        }
    } else {
        if (!record.sharedMembers
            || member_row(record, plan.identity.memberKey, plan.identity.accountSoid)
                   != plan.memberRow) {
            return false;
        }
        auto& held = record.memberLeases.held[plan.memberRow];
        if (plan.kind == MutationKind::grant) {
            if (!plan.requestedCount) {
                return false;
            }
            auto unavailable = held;
            const auto owned = member_lease_block(record.memberLeases.blocks[plan.memberRow],
                                                  record.memberLeases.blockWidth);
            for (std::size_t i = 0; i < unavailable.size(); ++i) {
                unavailable[i] |= ~owned[i];
            }
            expected = transactions::select_free(unavailable, plan.requestedCount);
        } else if (plan.kind == MutationKind::release) {
            expected = transactions::intersect(held, plan.returnedMask);
        } else {
            return false;
        }
        if (plan.mask != expected) {
            return false;
        }
        for (std::size_t i = 0; i < held.size(); ++i) {
            if (plan.kind == MutationKind::grant) {
                held[i] |= plan.mask[i];
            } else {
                held[i] &= ~plan.mask[i];
            }
        }
    }
    record.heldEntitySlots = aggregate_member_leases(record.memberLeases);
    return true;
}

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
        record.peerReservations = {};
        record.membership.epoch = membership::session_epoch(record.createdRevision);
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
    if (!prepared.shared
        && (prepared.identity != membership::Identity{}
            || prepared.identityGuard != membership::Identity{} || prepared.memberLease.valid
            || prepared.memberRow != kMemberLeaseRowCount || prepared.profileGeneration != 0)) {
        return false;
    }
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
    if (prepared.shared) {
        const bool applied = apply_shared(state, record, prepared);
        if (applied
            && (prepared.kind == MutationKind::join || !transactions::empty(prepared.mask))) {
            record.recordRevision = ++state.stateRevision;
            if (prepared.kind == MutationKind::join) {
                if (prepared.memberRow == 0) {
                    record.joinedRevision = state.stateRevision;
                } else {
                    record.coMembers[prepared.memberRow - 1].joinedRevision = state.stateRevision;
                }
            }
        }
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return applied;
    }
    if (record.sharedMembers) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
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
