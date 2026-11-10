#include <Windows.h>

#include <limits>

#include "../runtime/storage/internal.h"
#include "member_selection.h"
#include "runtime.h"
#include "transactions/internal.h"

namespace sunrise::state::activity {
/** Tests whether a nonzero activity-session id is still in the bounded table. */
bool contains(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    bool found = false;
    for (const SessionRecord& record : state.sessions) {
        if (record.occupied && record.sessionId == sessionId) {
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return found;
}

/**
 * Copies the binding of every committed session.
 * @param output Caller-owned storage.
 * @param count Receives the number written.
 * @return True when every committed session fit.
 */
bool snapshot_sessions(std::span<SessionBinding> output, std::size_t& count) noexcept {
    count = 0;
    bool complete = true;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    for (const SessionRecord& record : state.sessions) {
        if (!record.occupied || record.sessionId == kAbsentSessionId) {
            continue;
        }
        if (count == output.size()) {
            complete = false;
            break;
        }
        SessionBinding& binding = output[count++];
        binding = {};
        binding.destination = record.destination;
        binding.sessionId = record.sessionId;
        binding.createdRevision = record.createdRevision;
        binding.timeOrigin = record.timeOrigin;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return complete;
}

/** Copies the join state of every committed session, without any record body. */
bool snapshot_session_roster(std::span<SessionRosterRow> output, std::size_t& count) noexcept {
    count = 0;
    bool complete = true;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    for (const SessionRecord& record : state.sessions) {
        if (!record.occupied || record.sessionId == kAbsentSessionId) {
            continue;
        }
        if (count == output.size()) {
            complete = false;
            break;
        }
        SessionRosterRow& row = output[count++];
        row = {};
        row.binding.destination = record.destination;
        row.binding.sessionId = record.sessionId;
        row.binding.createdRevision = record.createdRevision;
        row.binding.timeOrigin = record.timeOrigin;
        row.memberKey = record.memberKey;
        for (std::size_t member = 0; member < kInvalidMemberRow; ++member) {
            if (const auto* identity = member_identity(record, member)) {
                row.presentMemberKey = member == 0 ? record.memberKey : identity->memberKey;
                break;
            }
        }
        row.joinIdentity =
            record.membership.hasIdentity ? record.membership.identity.joinIdentity : 0;
        row.joinedRevision = record.joinedRevision;
        row.joined = record.joined && record.joinedRevision != kInvalidRevision;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return complete;
}

/** Copies the binding of every session generation held by a live owner. */
bool snapshot_retained_bindings(std::span<SessionBinding> output, std::size_t& count) noexcept {
    count = 0;
    bool complete = true;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    for (const SessionRecord& record : state.sessions) {
        if (!record.occupied || record.sessionId == kAbsentSessionId
            || record.bindingRetainCount == 0) {
            continue;
        }
        if (count == output.size()) {
            complete = false;
            break;
        }
        SessionBinding& binding = output[count++];
        binding = {};
        binding.destination = record.destination;
        binding.sessionId = record.sessionId;
        binding.createdRevision = record.createdRevision;
        binding.timeOrigin = record.timeOrigin;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return complete;
}

/** Tests whether a committed activity-session id has finished a join. */
bool is_joined(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    bool joined = false;
    for (const SessionRecord& record : state.sessions) {
        if (record.occupied && record.sessionId == sessionId) {
            joined = record.joined && record.joinedRevision != kInvalidRevision;
            break;
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return joined;
}

/** Copies the exact destination and generation of one committed session. */
bool snapshot_binding(std::uint64_t sessionId, SessionBinding& output) noexcept {
    output = {};
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t slot = transactions::find_session(state, sessionId);
    const bool found = slot < kSessionCapacity;
    if (found) {
        const SessionRecord& record = state.sessions[slot];
        output.destination = record.destination;
        output.sessionId = record.sessionId;
        output.createdRevision = record.createdRevision;
        output.timeOrigin = record.timeOrigin;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return found;
}

/** Tests whether the exact bound record generation is still committed. */
bool binding_matches(const SessionBinding& binding) noexcept {
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t slot = transactions::find_session(state, binding.sessionId);
    const bool matches =
        slot < kSessionCapacity && transactions::record_matches(state.sessions[slot], binding);
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return matches;
}

/** Retains an exact record generation against release and allocator eviction. */
bool retain_binding(const SessionBinding& binding) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t slot = transactions::find_session(state, binding.sessionId);
    bool retained =
        slot < kSessionCapacity && transactions::record_matches(state.sessions[slot], binding);
    if (retained) {
        SessionRecord& record = state.sessions[slot];
        retained = record.bindingRetainCount
                   != (std::numeric_limits<decltype(record.bindingRetainCount)>::max)();
        if (retained) {
            ++record.bindingRetainCount;
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return retained;
}

/** Releases one retain only when the exact record generation still matches. */
void release_binding(const SessionBinding& binding) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t slot = transactions::find_session(state, binding.sessionId);
    if (slot < kSessionCapacity && transactions::record_matches(state.sessions[slot], binding)
        && state.sessions[slot].bindingRetainCount != 0) {
        --state.sessions[slot].bindingRetainCount;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

} // namespace sunrise::state::activity
