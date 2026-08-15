#include <Windows.h>

#include "../../../runtime/storage/internal.h"
#include "../activity_membership_query.h"
#include "internal.h"

namespace sunrise::state::activity::membership {

/** Prepares sparse host-state changes for one joined activity session. */
bool prepare_authoritative(std::uint64_t sessionId,
                           const AuthoritativeUpdate& update,
                           PendingMutation& mutation) noexcept {
    mutation = {};
    if (sessionId == kAbsentSessionId) {
        return false;
    }

    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const auto& root = runtime::storage::g_state;
    PendingMutation prepared{};
    const SessionRecord* record =
        transactions::prepare_base(root.activity, root.account.primarySoid, sessionId, prepared);
    bool ready = record != nullptr;
    if (ready) {
        const MembershipState merged = transactions::merge(record->membership, update);
        const bool changed = !transactions::equal_authoritative(record->membership, merged);
        const bool revisionExhausted =
            root.activity.stateRevision == activity::kMaximumRevision
            || (record->membership.hasIdentity
                && record->membership.revision == kMaximumMembershipRevision);
        if (changed && revisionExhausted) {
            ready = false;
        } else {
            prepared.authoritativeInput = update;
            prepared.authoritativeGuard = update;
            prepared.kind = MutationKind::authoritative;
            prepared.changesState = changed;
            prepared.movesRegion = transactions::moves_region(record->membership, merged);
            prepared.movesTransitionToken =
                transactions::moves_transition_token(record->membership, merged);
            prepared.hasSnapshot = changed && record->membership.hasIdentity;
            if (prepared.hasSnapshot) {
                prepared.snapshot =
                    transactions::make_snapshot(merged, merged.identity, merged.revision + 1U);
            }
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    if (!ready) {
        return false;
    }
    mutation = prepared;
    return true;
}

} // namespace sunrise::state::activity::membership
