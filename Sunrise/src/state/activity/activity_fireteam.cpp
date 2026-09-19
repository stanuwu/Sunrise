#include <Windows.h>

#include "../account/account_context.h"
#include "../account/public_profiles.h"
#include "../runtime/storage/internal.h"
#include "fireteam.h"

namespace sunrise::state::activity::fireteam {
bool request_join(std::uint64_t joiner, std::uint64_t target) noexcept {
    if (!joiner || !target || joiner == target || joiner != account_primary_soid(bound_account())) {
        return false;
    }
    AccountHandle other{};
    if (!account::profiles::find(target, other) || !account::profiles::selected_character(other)) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    // Recorded intent is not a membership fact, so it advances no State revision and cannot
    // retire a transaction that is already in flight.
    const bool ready = runtime::storage::g_state.activity.fireteams.request(joiner, target);
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return ready;
}

bool connected(std::uint64_t first, std::uint64_t second) noexcept {
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const bool value = runtime::storage::g_state.activity.fireteams.connected(first, second);
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return value;
}

std::uint64_t representative(std::uint64_t account) noexcept {
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const auto value = runtime::storage::g_state.activity.fireteams.representative(account);
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return value;
}

std::uint64_t revision() noexcept {
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const auto value = runtime::storage::g_state.activity.fireteams.revision();
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return value;
}

bool depart(std::uint64_t account) noexcept {
    if (!account || account != account_primary_soid(bound_account())) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    auto& state = runtime::storage::g_state.activity;
    const bool changed = state.stateRevision != kMaximumRevision && state.fireteams.depart(account);
    if (changed) {
        ++state.stateRevision;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return changed;
}

bool native_solo_split(const social::NativePresence& before,
                       const social::NativePresence& after) noexcept {
    return before.published && after.published && before.hasGroup && after.hasGroup
           && before.characterSoid != 0 && before.characterSoid == after.characterSoid
           && before.memberCount > 1 && after.memberCount == 1 && before.groupKey != after.groupKey;
}
} // namespace sunrise::state::activity::fireteam
