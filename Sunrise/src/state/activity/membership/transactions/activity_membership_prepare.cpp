#include <Windows.h>

#include <cstdint>

#include "../../../runtime/storage/internal.h"
#include "../activity_membership_query.h"
#include "internal.h"

namespace sunrise::state::activity::membership {

/** Prepares one exact identity for a joined activity session. */
bool prepare_identity(std::uint64_t sessionId,
                      const Identity& identity,
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
    bool ready = record != nullptr && transactions::valid_identity(identity, record->memberKey);
    if (ready) {
        const bool changed = !record->membership.hasIdentity
                             || !transactions::equal(record->membership.identity, identity);
        if (changed
            && (root.activity.stateRevision == activity::kMaximumRevision
                || record->membership.revision == kMaximumMembershipRevision)) {
            ready = false;
        } else {
            const std::uint32_t revision =
                !changed ? record->membership.revision
                         : (record->membership.hasIdentity ? record->membership.revision + 1U
                                                           : kInitialRevision);
            prepared.snapshot = transactions::make_snapshot(record->membership, identity, revision);
            prepared.identityGuard = identity;
            prepared.kind = MutationKind::identity;
            prepared.hasSnapshot = true;
            prepared.changesState = changed;
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    if (!ready) {
        return false;
    }
    mutation = prepared;
    return true;
}

/** Captures the current membership snapshot without changing stored State. */
bool prepare_refresh(std::uint64_t sessionId,
                     std::uint32_t requestedRevision,
                     std::int32_t bubbleIndex,
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
    if (record != nullptr) {
        if (record->membership.hasIdentity) {
            prepared.snapshot = transactions::make_snapshot(
                record->membership, record->membership.identity, record->membership.revision);
            prepared.hasSnapshot = true;
        }
        prepared.requestedRevision = requestedRevision;
        prepared.bubbleIndex = bubbleIndex;
        prepared.refreshRequestGuard = transactions::refresh_guard(requestedRevision, bubbleIndex);
        prepared.kind = MutationKind::refresh;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    if (record == nullptr) {
        return false;
    }
    mutation = prepared;
    return true;
}

/** Prepares one exact membership-revision advance without changing stored State. */
bool prepare_republish(std::uint64_t sessionId, PendingMutation& mutation) noexcept {
    mutation = {};
    if (sessionId == kAbsentSessionId) {
        return false;
    }

    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const auto& root = runtime::storage::g_state;
    PendingMutation prepared{};
    const SessionRecord* record =
        transactions::prepare_base(root.activity, root.account.primarySoid, sessionId, prepared);
    const bool ready = record != nullptr && record->membership.hasIdentity
                       && root.activity.stateRevision != activity::kMaximumRevision
                       && record->membership.revision != kMaximumMembershipRevision;
    if (ready) {
        prepared.snapshot = transactions::make_snapshot(
            record->membership, record->membership.identity, record->membership.revision + 1U);
        prepared.kind = MutationKind::republish;
        prepared.hasSnapshot = true;
        prepared.changesState = true;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    if (!ready) {
        return false;
    }
    mutation = prepared;
    return true;
}

/** Prepares an acknowledgement mark for the current membership revision. */
bool prepare_acknowledgement(std::uint64_t sessionId,
                             std::uint32_t revision,
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
        const bool changed = record->membership.hasIdentity
                             && revision == record->membership.revision
                             && revision != record->membership.acknowledgedRevision;
        if (changed && root.activity.stateRevision == activity::kMaximumRevision) {
            ready = false;
        } else {
            prepared.acknowledgement = revision;
            prepared.kind = MutationKind::acknowledgement;
            prepared.changesState = changed;
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
