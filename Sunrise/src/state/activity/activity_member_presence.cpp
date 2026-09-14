#include "../runtime/storage/internal.h"
#include "member_presence.h"
#include "member_selection.h"
#include "transactions/internal.h"

namespace sunrise::state::activity::presence {
namespace {
const membership::MembershipState* owned(const SessionRecord& record,
                                         std::uint64_t account,
                                         std::uint64_t character,
                                         std::uint64_t* joinedRevision = nullptr) noexcept {
    if (!record.occupied || !account || !character) {
        return nullptr;
    }
    const auto row = member_row(record, 0, account);
    const auto* identity = member_identity(record, row);
    if (!identity || identity->accountSoid != account || identity->opaqueSoid != character) {
        return nullptr;
    }
    if (joinedRevision) {
        *joinedRevision =
            row == 0 ? record.joinedRevision : record.coMembers[row - 1].joinedRevision;
    }
    return member_state(record, row);
}
void copy(const membership::MembershipState& state, membership::ClientPlacement& output) noexcept {
    output.region = state.region.index;
    output.currentRegion = state.currentRegion.index;
    output.bubble = state.bubble;
    output.bubbleRevision = state.bubbleRevision;
    output.entered = state.entered;
}
} // namespace

bool placement(const SessionBinding& binding,
               std::uint64_t account,
               std::uint64_t character,
               membership::ClientPlacement& output) noexcept {
    output = {};
    bool found = false;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    for (const auto& record : runtime::storage::g_state.activity.sessions) {
        if (!transactions::record_matches(record, binding)) {
            continue;
        }
        if (const auto* report = owned(record, account, character)) {
            copy(*report, output);
            found = true;
        }
        break;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return found;
}

bool latest_placement(std::uint64_t account,
                      std::uint64_t character,
                      membership::ClientPlacement& output) noexcept {
    output = {};
    std::uint64_t newest = 0;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    for (const auto& record : runtime::storage::g_state.activity.sessions) {
        std::uint64_t joinedRevision{};
        const auto* report = owned(record, account, character, &joinedRevision);
        if (!report || joinedRevision <= newest
            || (report->region.index < 0 && report->currentRegion.index < 0)) {
            continue;
        }
        copy(*report, output);
        newest = joinedRevision;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return newest != 0;
}

std::uint64_t seat_session(std::uint64_t account, std::uint64_t character) noexcept {
    std::uint64_t newest = 0;
    std::uint64_t sessionId = 0;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    for (const auto& record : runtime::storage::g_state.activity.sessions) {
        // Published ids include the host account's prefix. The member's own join revision orders
        // seats across hosts, including a guest joining an older activity before reporting arrival.
        std::uint64_t joinedRevision{};
        if (owned(record, account, character, &joinedRevision) == nullptr
            || joinedRevision <= newest) {
            continue;
        }
        newest = joinedRevision;
        sessionId = record.sessionId;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return sessionId;
}
} // namespace sunrise::state::activity::presence
