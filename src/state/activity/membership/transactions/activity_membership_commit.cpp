#include <Windows.h>

#include <cstdint>

#include "../../../runtime/storage/internal.h"
#include "../activity_membership_query.h"
#include "internal.h"

namespace sunrise::state::activity::membership {
namespace {

/**
 * Applies one client identity operation to current State.
 * @param state Activity State held under the root write lock.
 * @param record Target joined session record.
 * @param prepared Identity plan, consumed here.
 * @return True when the candidate matches and commits, or is unchanged.
 */
[[nodiscard]] bool commit_identity(ActivityState& state,
                                   SessionRecord& record,
                                   const PendingMutation& prepared) noexcept {
    if (!prepared.hasSnapshot
        || !transactions::equal(prepared.snapshot.identity, prepared.identityGuard)
        || !transactions::valid_identity(prepared.snapshot.identity, record.memberKey)) {
        return false;
    }
    // Current State decides the outcome. Comparing it against the prepared plan and refusing on a
    // difference would drop the identity whenever State moved between prepare and commit, so no
    // membership would ever publish.
    const bool changed =
        !record.membership.hasIdentity
        || !transactions::equal(record.membership.identity, prepared.snapshot.identity);
    if (changed
        && (state.stateRevision == activity::kMaximumRevision
            || record.membership.revision == kMaximumMembershipRevision)) {
        return false;
    }
    if (!changed) {
        return true;
    }
    const std::uint32_t revision =
        record.membership.hasIdentity ? record.membership.revision + 1U : kInitialRevision;

    MembershipState updated = record.membership;
    if (!updated.hasTransitionToken) {
        updated.transitionToken = kInitialTransitionToken;
        updated.hasTransitionToken = true;
    }
    updated.identity = prepared.snapshot.identity;
    updated.revision = revision;
    updated.acknowledgedRevision = kAbsentRevision;
    updated.hasIdentity = true;
    record.membership = updated;
    transactions::publish_change(state, record);
    return true;
}

/**
 * Checks one read-only refresh plan against current State.
 * @param record Target joined session record.
 * @param prepared Refresh plan, consumed here.
 * @return True when the request guard and snapshot still match.
 */
[[nodiscard]] bool commit_refresh(const SessionRecord& record,
                                  const PendingMutation& prepared) noexcept {
    if (prepared.refreshRequestGuard
        != transactions::refresh_guard(prepared.requestedRevision, prepared.bubbleIndex)) {
        return false;
    }
    if (!record.membership.hasIdentity) {
        return !prepared.hasSnapshot;
    }
    if (!prepared.hasSnapshot) {
        return false;
    }
    const Snapshot expected = transactions::make_snapshot(
        record.membership, record.membership.identity, record.membership.revision);
    return transactions::equal(prepared.snapshot, expected);
}

/**
 * Applies one membership acknowledgement to current State.
 * @param state Activity State held under the root write lock.
 * @param record Target joined session record.
 * @param prepared Acknowledgement plan, consumed here.
 * @return True when the mark commits or the revision is a no-op.
 */
[[nodiscard]] bool commit_acknowledgement(ActivityState& state,
                                          SessionRecord& record,
                                          const PendingMutation& prepared) noexcept {
    const bool changed = record.membership.hasIdentity
                         && prepared.acknowledgement == record.membership.revision
                         && prepared.acknowledgement != record.membership.acknowledgedRevision;
    if (changed && state.stateRevision == activity::kMaximumRevision) {
        return false;
    }
    if (changed) {
        record.membership.acknowledgedRevision = prepared.acknowledgement;
        transactions::publish_change(state, record);
    }
    return true;
}

} // namespace

/** Commits one identity, authoritative, refresh, or acknowledgement operation. */
bool commit(PendingMutation& mutation) noexcept {
    const PendingMutation prepared = mutation;
    mutation = {};
    if (!prepared.prepared || prepared.kind == MutationKind::none
        || prepared.sessionId == kAbsentSessionId
        || prepared.expectedStateRevision == kInvalidRevision
        || prepared.expectedRecordRevision == kInvalidRevision
        || prepared.targetSlot >= kSessionCapacity) {
        return false;
    }

    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    auto& root = runtime::storage::g_state;
    ActivityState& state = root.activity;
    SessionRecord& record = state.sessions[prepared.targetSlot];
    bool committed = state.stateRevision == prepared.expectedStateRevision && record.occupied
                     && record.joined && record.joinedRevision != kInvalidRevision
                     && record.sessionId == prepared.sessionId
                     && record.recordRevision == prepared.expectedRecordRevision
                     && root.account.primarySoid == prepared.expectedPrimarySoid;
    if (committed && prepared.kind == MutationKind::identity) {
        committed = commit_identity(state, record, prepared);
    } else if (committed && prepared.kind == MutationKind::authoritative) {
        committed = transactions::commit_authoritative(state, record, prepared);
    } else if (committed && prepared.kind == MutationKind::refresh) {
        committed = commit_refresh(record, prepared);
    } else if (committed && prepared.kind == MutationKind::acknowledgement) {
        committed = commit_acknowledgement(state, record, prepared);
    } else {
        committed = false;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return committed;
}

} // namespace sunrise::state::activity::membership
