#include <Windows.h>

#include "../../account/account_context.h"
#include "../../runtime/storage/internal.h"
#include "../entity_slots/runtime.h"
#include "../transactions/internal.h"
#include "activity_membership_query.h"
#include "member_directory_builder.h"
#include "transactions/internal.h"

namespace sunrise::state::activity::membership {
/** Builds the native join burst without admitting a row or replacing any other client's report. */
bool prepare_join_snapshot(const entity_slots::PendingMutation& join,
                           PendingMutation& mutation) noexcept {
    mutation = {};
    const auto& identity = join.identity;
    if (!join.prepared || !join.shared || join.kind != entity_slots::MutationKind::join
        || !join.sessionId || identity.accountSoid != account_primary_soid(bound_account())) {
        return false;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const auto& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, join.sessionId);
    bool ready = target != kInvalidSessionSlot && target == join.targetSlot
                 && state.stateRevision == join.expectedStateRevision;
    if (ready) {
        const auto& record = state.sessions[target];
        const auto row = join_member_row(record, identity, join.sourceAuthorized);
        ready = row != kInvalidMemberRow && row == join.memberRow
                && record.recordRevision == join.expectedRecordRevision;
        if (ready) {
            MembershipState seed{};
            seed.epoch = session_epoch(record.createdRevision);
            if (const auto* member = member_state(record, row); member && !join.replacesMemberKey) {
                seed = *member;
            }
            const auto revision = seed.hasIdentity ? seed.revision : kInitialRevision;
            // The session this body is about. The per-connection cursor records it with the
            // revision, so the keepalive can tell the burst's body from one it still owes.
            mutation.sessionId = join.sessionId;
            mutation.snapshot = transactions::make_snapshot(seed, identity, revision);
            mutation.memberDirectory =
                member_directory(record, identity, &join.peerReservationsAfter);
            mutation.hasSnapshot = mutation.memberDirectory.valid;
            ready = mutation.hasSnapshot;
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    if (!ready) {
        mutation = {};
    }
    return ready;
}
} // namespace sunrise::state::activity::membership
