#pragma once

#include "definition.h"

namespace sunrise::state::activity {
/** Sentinel row past every real member slot; also the loop bound for scanning them. */
inline constexpr std::size_t kInvalidMemberRow = entity_slots::kMemberLeaseRowCount;

/** @return True when `row` is an occupied member slot; row 0 is the record's own primary. */
[[nodiscard]] inline bool member_joined(const SessionRecord& record, std::size_t row) noexcept {
    if (row == 0) {
        return record.joined && (!record.sharedMembers || (record.memberLeases.joinedRows & 1U));
    }
    return row < kInvalidMemberRow && record.coMembers[row - 1].joined;
}
/** @return The row's identity, or null for a row that is not joined. */
[[nodiscard]] inline const membership::Identity* member_identity(const SessionRecord& record,
                                                                 std::size_t row) noexcept {
    if (!member_joined(record, row)) {
        return nullptr;
    }
    return row == 0 ? &record.primaryIdentity : &record.coMembers[row - 1].identity;
}
/** @return The row's membership state, or null for a row that is not joined. */
[[nodiscard]] inline membership::MembershipState* member_state(SessionRecord& record,
                                                               std::size_t row) noexcept {
    if (!member_joined(record, row)) {
        return nullptr;
    }
    return row == 0 ? &record.membership : &record.coMembers[row - 1].membership;
}
/** @return The row's membership state, or null for a row that is not joined. */
[[nodiscard]] inline const membership::MembershipState* member_state(const SessionRecord& record,
                                                                     std::size_t row) noexcept {
    if (!member_joined(record, row)) {
        return nullptr;
    }
    return row == 0 ? &record.membership : &record.coMembers[row - 1].membership;
}
/** An unknown native key is never redirected to the primary player's state. */
[[nodiscard]] inline std::size_t
member_row(const SessionRecord& record, std::uint64_t key, std::uint64_t account) noexcept {
    if (!record.sharedMembers) {
        return record.joined && (!key || record.memberKey == key) ? 0 : kInvalidMemberRow;
    }
    if (!account) {
        return kInvalidMemberRow;
    }
    for (std::size_t row = 0; row < kInvalidMemberRow; ++row) {
        const auto* identity = member_identity(record, row);
        if (identity && identity->accountSoid == account && (!key || identity->memberKey == key)) {
            return row;
        }
    }
    return kInvalidMemberRow;
}
/**
 * Which row `identity` may occupy. An un-joined record always grants row 0; a rejoin reuses
 * the row already carrying the same member key or account, if the rest of the identity still
 * matches. A free peer row is granted only when a peer reservation, a launch owner, or, as a
 * last resort, `sourceAuthorized` names this identity.
 * @return `kInvalidMemberRow` when the member key or account collides with a different
 * identity anywhere in the record, or nothing grants a row.
 */
[[nodiscard]] inline std::size_t join_member_row(const SessionRecord& record,
                                                 const membership::Identity& identity,
                                                 bool sourceAuthorized = false) noexcept {
    if (!identity.memberKey || !identity.accountSoid || !identity.opaqueSoid) {
        return kInvalidMemberRow;
    }
    if (!record.joined) {
        return 0;
    }
    if (!record.sharedMembers) {
        return kInvalidMemberRow;
    }
    const auto& reservedPrimary = record.peerReservations.primary;
    if (reservedPrimary.memberKey == identity.memberKey
        && reservedPrimary.accountSoid != identity.accountSoid) {
        return kInvalidMemberRow;
    }
    for (std::size_t row = 0; row < kInvalidMemberRow; ++row) {
        const auto* existing = member_identity(record, row);
        if (existing && existing->memberKey == identity.memberKey
            && existing->accountSoid != identity.accountSoid) {
            return kInvalidMemberRow;
        }
    }
    for (const auto& peer : record.peerReservations.peers) {
        if (peer.memberKey == identity.memberKey && peer.accountSoid != identity.accountSoid) {
            return kInvalidMemberRow;
        }
    }
    std::size_t available = kInvalidMemberRow;
    for (std::size_t row = 0; row < kInvalidMemberRow; ++row) {
        const auto* existing = member_identity(record, row);
        if (!existing) {
            if (row == 0 && record.primaryIdentity.accountSoid == identity.accountSoid) {
                return 0;
            }
            if (row != 0 && available == kInvalidMemberRow) {
                available = row;
            }
            continue;
        }
        if (existing->memberKey == identity.memberKey
            || existing->accountSoid == identity.accountSoid) {
            return existing->accountSoid == identity.accountSoid
                           && existing->opaqueSoid == identity.opaqueSoid
                       ? row
                       : kInvalidMemberRow;
        }
    }
    for (const auto& peer : record.peerReservations.peers) {
        if (peer == identity) {
            return available;
        }
    }
    for (const auto& owner : record.launchOwners) {
        if (owner.accountSoid == identity.accountSoid
            && owner.characterSoid == identity.opaqueSoid) {
            return available;
        }
    }
    return sourceAuthorized ? available : kInvalidMemberRow;
}
} // namespace sunrise::state::activity
