#include <Windows.h>

#include <cstdint>

#include "../../../account/account_context.h"
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

    const auto primarySoid = account_primary_soid(bound_account());
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const auto& root = runtime::storage::g_state;
    PendingMutation prepared{};
    const SessionRecord* record =
        transactions::prepare_base(root.activity, primarySoid, sessionId, prepared);
    const auto* member = record ? member_state(*record, prepared.memberRow) : nullptr;
    bool ready = record != nullptr
                 && transactions::valid_identity(identity, prepared.expectedMemberKey)
                 && (!record->sharedMembers
                     || (identity.accountSoid == primarySoid
                         && identity.opaqueSoid
                                == member_identity(*record, prepared.memberRow)->opaqueSoid));
    if (ready) {
        const bool changed =
            !member->hasIdentity || !transactions::equal(member->identity, identity);
        if (changed
            && (root.activity.stateRevision == activity::kMaximumRevision
                || member->revision == kMaximumMembershipRevision)) {
            ready = false;
        } else {
            const std::uint32_t revision =
                !changed ? member->revision
                         : (member->hasIdentity ? member->revision + 1U : kInitialRevision);
            prepared.snapshot = transactions::make_snapshot(*member, identity, revision);
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

    const auto primarySoid = account_primary_soid(bound_account());
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const auto& root = runtime::storage::g_state;
    PendingMutation prepared{};
    const SessionRecord* record =
        transactions::prepare_base(root.activity, primarySoid, sessionId, prepared);
    const auto* member = record ? member_state(*record, prepared.memberRow) : nullptr;
    if (record != nullptr) {
        if (member->hasIdentity) {
            prepared.snapshot =
                transactions::make_snapshot(*member, member->identity, member->revision);
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

    const auto primarySoid = account_primary_soid(bound_account());
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const auto& root = runtime::storage::g_state;
    PendingMutation prepared{};
    const SessionRecord* record =
        transactions::prepare_base(root.activity, primarySoid, sessionId, prepared);
    const auto* member = record ? member_state(*record, prepared.memberRow) : nullptr;
    const bool ready = record != nullptr && member->hasIdentity
                       && root.activity.stateRevision != activity::kMaximumRevision
                       && member->revision != kMaximumMembershipRevision;
    if (ready) {
        prepared.snapshot =
            transactions::make_snapshot(*member, member->identity, member->revision + 1U);
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

    const auto primarySoid = account_primary_soid(bound_account());
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const auto& root = runtime::storage::g_state;
    PendingMutation prepared{};
    const SessionRecord* record =
        transactions::prepare_base(root.activity, primarySoid, sessionId, prepared);
    const auto* member = record ? member_state(*record, prepared.memberRow) : nullptr;
    bool ready = record != nullptr;
    if (ready) {
        const bool changed = member->hasIdentity && revision == member->revision
                             && revision != member->acknowledgedRevision;
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
