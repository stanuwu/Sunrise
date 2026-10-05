#pragma once

#include "../../member_selection.h"
#include "../../transactions/internal.h"

namespace sunrise::state::activity::entity_slots::transactions {
/**
 * A native host directory carries the exact source generation, whose admitted party owns
 * this join.
 */
[[nodiscard]] inline const SessionRecord*
authorized_source(const ActivityState& state,
                  const SessionBinding& source,
                  const membership::Identity& identity) noexcept {
    const auto slot = activity::transactions::find_session(state, source.sessionId);
    if (slot == kInvalidSessionSlot) {
        return nullptr;
    }
    const auto& record = state.sessions[slot];
    if (!record.joined || !record.sharedMembers
        || !activity::transactions::record_matches(record, source)) {
        return nullptr;
    }
    for (std::size_t row = 0; row < kInvalidMemberRow; ++row) {
        const auto* member = member_identity(record, row);
        if (member && member->accountSoid == identity.accountSoid
            && member->opaqueSoid == identity.opaqueSoid) {
            return &record;
        }
    }
    for (const auto& peer : record.peerReservations.peers) {
        if (peer.memberKey && peer.accountSoid == identity.accountSoid
            && peer.opaqueSoid == identity.opaqueSoid) {
            return &record;
        }
    }
    const auto& primary = record.peerReservations.primary;
    if (primary.memberKey && primary.accountSoid == identity.accountSoid
        && primary.opaqueSoid == identity.opaqueSoid) {
        return &record;
    }
    return nullptr;
}

/**
 * Records `identity` at `memberRow` in a copy of `record`'s peer reservations, without
 * committing it. Row 0 clears the standing primary-slot reservation instead of writing a
 * peer entry; elsewhere, a peer entry keyed by `identity.memberKey` or by `previousKey` (a
 * lease hand-off) is reused only if its account and opaque ids still match.
 * @return False when the identity conflicts with a differently-keyed peer or no peer slot is
 * free; `after` is left as the unmodified original roster in that case.
 */
[[nodiscard]] inline bool join_roster(const SessionRecord& record,
                                      const membership::Identity& identity,
                                      std::size_t memberRow,
                                      reservations::Roster& after,
                                      std::uint64_t previousKey = 0) noexcept {
    after = record.peerReservations;
    if (memberRow == 0) {
        after.primary = {};
        return true;
    }
    membership::Identity* available{};
    for (auto& peer : after.peers) {
        if (peer.memberKey == identity.memberKey || peer.accountSoid == identity.accountSoid) {
            if ((peer.memberKey != identity.memberKey && peer.memberKey != previousKey)
                || peer.accountSoid != identity.accountSoid
                || peer.opaqueSoid != identity.opaqueSoid) {
                return false;
            }
            peer = identity;
            return true;
        }
        if (!peer.memberKey && !available) {
            available = &peer;
        }
    }
    if (!available) {
        return false;
    }
    *available = identity;
    return true;
}
} // namespace sunrise::state::activity::entity_slots::transactions
