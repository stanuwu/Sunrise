#include <Windows.h>

#include "../../../account/account_context.h"
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

    const auto primarySoid = account_primary_soid(bound_account());
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const auto& root = runtime::storage::g_state;
    PendingMutation prepared{};
    const SessionRecord* record =
        transactions::prepare_base(root.activity, primarySoid, sessionId, prepared);
    const auto* member = record ? member_state(*record, prepared.memberRow) : nullptr;
    bool ready = record != nullptr;
    if (ready) {
        const MembershipState merged = transactions::merge(*member, update);
        const bool changed = !transactions::equal_authoritative(*member, merged);
        const bool revisionExhausted =
            root.activity.stateRevision == activity::kMaximumRevision
            || (member->hasIdentity && member->revision == kMaximumMembershipRevision);
        if (changed && revisionExhausted) {
            ready = false;
        } else {
            prepared.authoritativeInput = update;
            prepared.authoritativeGuard = update;
            prepared.kind = MutationKind::authoritative;
            prepared.changesState = changed;
            prepared.movesRegion = transactions::moves_region(*member, merged);
            prepared.movesTransitionToken = transactions::moves_transition_token(*member, merged);
            prepared.hasSnapshot = changed && member->hasIdentity;
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
