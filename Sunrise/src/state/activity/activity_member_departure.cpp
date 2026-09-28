#include <Windows.h>

#include "../account/account_context.h"
#include "../runtime/storage/internal.h"
#include "member_departure.h"
#include "member_mutation.h"
#include "transactions/internal.h"

namespace sunrise::state::activity {
bool joined_member_set(const SessionBinding& binding,
                       std::uint64_t memberKey,
                       JoinedMemberSet& output) noexcept {
    output = {};
    const auto account = account_primary_soid(bound_account());
    bool found{};
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const auto& state = runtime::storage::g_state.activity;
    const auto target = transactions::find_session(state, binding.sessionId);
    if (target != kInvalidSessionSlot) {
        const auto& record = state.sessions[target];
        const auto row = member_row(record, memberKey, account);
        found = record.sharedMembers && memberKey && account
                && transactions::record_matches(record, binding) && row != kInvalidMemberRow;
        if (found) {
            for (std::size_t index = 0; index < output.keys.size(); ++index) {
                if (const auto* identity = member_identity(record, index)) {
                    output.keys[index] = identity->memberKey;
                }
            }
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return found;
}

bool pending_member_purge(const SessionBinding& binding,
                          std::uint64_t memberKey,
                          MemberPurge& output) noexcept {
    output = {};
    const auto account = account_primary_soid(bound_account());
    if (!memberKey || !account) {
        return false;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const auto& state = runtime::storage::g_state.activity;
    const auto target = transactions::find_session(state, binding.sessionId);
    if (target != kInvalidSessionSlot) {
        const auto& record = state.sessions[target];
        const auto row = member_row(record, memberKey, account);
        if (record.sharedMembers && transactions::record_matches(record, binding)
            && row != kInvalidMemberRow
            && entity_slots::slot_count(record.memberLeases.purgeOwed[row])) {
            output = {binding,
                      record.memberLeases.purgeOwed[row],
                      account,
                      memberKey,
                      record.memberLeases.purgeSequence[row]};
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return output.memberKey != 0;
}

bool commit_member_purge(const MemberPurge& pending) noexcept {
    if (!pending.memberKey || pending.accountSoid != account_primary_soid(bound_account())
        || !entity_slots::slot_count(pending.slots)) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    auto& state = runtime::storage::g_state.activity;
    const auto target = transactions::find_session(state, pending.binding.sessionId);
    bool committed{};
    if (target != kInvalidSessionSlot && state.stateRevision != kMaximumRevision) {
        auto& record = state.sessions[target];
        const auto row = member_row(record, pending.memberKey, pending.accountSoid);
        if (record.sharedMembers && transactions::record_matches(record, pending.binding)
            && row != kInvalidMemberRow && record.memberLeases.purgeOwed[row] == pending.slots
            && record.memberLeases.purgeSequence[row] == pending.replicationSequence) {
            auto& member = *member_state(record, row);
            if (member.hasIdentity && member.revision != membership::kMaximumMembershipRevision) {
                // This fresh revision can only be acknowledged after the queued purge. A receipt
                // for an earlier membership cannot release these slots for another incarnation.
                ++member.revision;
                member.acknowledgedRevision = membership::kAbsentRevision;
                record.memberLeases.purgeSequence[row] = 0;
                record.memberLeases.purgeOwed[row] = {};
                record.memberLeases.purgeRevision[row] = member.revision;
                record.recordRevision = ++state.stateRevision;
                committed = true;
            }
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return committed;
}

bool replication_sequence(const SessionBinding& binding, std::uint64_t& sequence) noexcept {
    sequence = 0;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const auto& state = runtime::storage::g_state.activity;
    const auto target = transactions::find_session(state, binding.sessionId);
    const bool valid = target != kInvalidSessionSlot
                       && transactions::record_matches(state.sessions[target], binding);
    if (valid) {
        sequence = state.sessions[target].memberLeases.replicationSequence;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return valid;
}

bool advance_replication_sequence(const SessionBinding& binding,
                                  std::uint64_t expected,
                                  const MemberPurge* deliveredPurge) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    auto& state = runtime::storage::g_state.activity;
    const auto target = transactions::find_session(state, binding.sessionId);
    bool valid = target != kInvalidSessionSlot
                 && transactions::record_matches(state.sessions[target], binding)
                 && state.sessions[target].memberLeases.replicationSequence == expected
                 && expected != (std::numeric_limits<std::uint64_t>::max)()
                 && state.stateRevision != kMaximumRevision;
    std::size_t row = kInvalidMemberRow;
    if (valid && deliveredPurge && deliveredPurge->memberKey) {
        const auto& record = state.sessions[target];
        row = member_row(record, deliveredPurge->memberKey, deliveredPurge->accountSoid);
        valid = deliveredPurge->accountSoid == account_primary_soid(bound_account())
                && transactions::record_matches(record, deliveredPurge->binding)
                && row != kInvalidMemberRow
                && record.memberLeases.purgeOwed[row] == deliveredPurge->slots
                && record.memberLeases.purgeSequence[row] == deliveredPurge->replicationSequence
                && member_state(record, row)->revision != membership::kMaximumMembershipRevision;
    }
    if (valid) {
        auto& record = state.sessions[target];
        if (row != kInvalidMemberRow) {
            auto& member = *member_state(record, row);
            ++member.revision;
            member.acknowledgedRevision = membership::kAbsentRevision;
            record.memberLeases.purgeOwed[row] = {};
            record.memberLeases.purgeSequence[row] = 0;
            record.memberLeases.purgeRevision[row] = member.revision;
        }
        ++record.memberLeases.replicationSequence;
        record.recordRevision = ++state.stateRevision;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return valid;
}

bool depart_member(const SessionBinding& binding,
                   std::uint64_t accountSoid,
                   std::uint64_t memberKey) noexcept {
    if (!memberKey || !accountSoid || accountSoid != account_primary_soid(bound_account())) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    auto& state = runtime::storage::g_state.activity;
    const auto target = transactions::find_session(state, binding.sessionId);
    bool changed = target != kInvalidSessionSlot && state.stateRevision != kMaximumRevision;
    if (changed) {
        auto& record = state.sessions[target];
        const auto row = member_row(record, memberKey, accountSoid);
        changed = record.sharedMembers && transactions::record_matches(record, binding)
                  && row != kInvalidMemberRow && can_republish_members(record, row);
        if (changed) {
            for (std::size_t survivor = 0; survivor < kInvalidMemberRow; ++survivor) {
                const auto* member = member_state(record, survivor);
                if (survivor != row && member && member->hasIdentity
                    && member->epoch == membership::kMaximumPeerTableEpoch) {
                    changed = false;
                    break;
                }
            }
        }
        if (changed) {
            remove_joined_member(record, row);
            for (auto& peer : record.peerReservations.peers) {
                if (peer.memberKey == memberKey && peer.accountSoid == accountSoid) {
                    peer = {};
                }
            }
            auto& primary = record.peerReservations.primary;
            if (primary.memberKey == memberKey && primary.accountSoid == accountSoid) {
                primary = {};
            }
            republish_members(record, row);
            // A disconnect can remove this peer before the native host sends its release.
            // Changing the peer-table generation clears the survivor's cached reservation keys;
            // a revision-only removal leaves the departed key suppressing its next join request.
            for (std::size_t survivor = 0; survivor < kInvalidMemberRow; ++survivor) {
                auto* member = member_state(record, survivor);
                if (member && member->hasIdentity) {
                    ++member->epoch;
                }
            }
            record.recordRevision = ++state.stateRevision;
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return changed;
}
} // namespace sunrise::state::activity
