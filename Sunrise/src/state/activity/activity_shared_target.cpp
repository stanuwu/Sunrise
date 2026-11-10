#include <Windows.h>

#include "../account/account_context.h"
#include "../runtime/storage/internal.h"
#include "member_selection.h"
#include "shared_target.h"
#include "transactions/internal.h"

namespace sunrise::state::activity {
bool shared_target(std::uint64_t sessionId,
                   const membership::Identity& identity,
                   SessionBinding& binding) noexcept {
    binding = {};
    if (!sessionId || identity.accountSoid != account_primary_soid(bound_account())) {
        return false;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const auto& state = runtime::storage::g_state.activity;
    const auto target = transactions::find_session(state, sessionId);
    bool ready = target != kInvalidSessionSlot;
    if (ready) {
        const auto& record = state.sessions[target];
        ready = record.sharedMembers && record.joined
                && join_member_row(record, identity) != kInvalidMemberRow;
        if (ready) {
            binding = {
                record.destination, record.sessionId, record.createdRevision, record.timeOrigin};
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return ready;
}
} // namespace sunrise::state::activity
