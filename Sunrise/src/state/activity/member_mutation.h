#pragma once

#include "member_selection.h"

namespace sunrise::state::activity {
/** A peer-visible change must be publishable to every remaining native recipient. */
[[nodiscard]] inline bool can_republish_members(const SessionRecord& record,
                                                std::size_t except = kInvalidMemberRow) noexcept {
    for (std::size_t row = 0; row < kInvalidMemberRow; ++row) {
        if (row == except) {
            continue;
        }
        const auto* member = member_state(record, row);
        if (member && member->hasIdentity
            && member->revision == membership::kMaximumMembershipRevision) {
            return false;
        }
    }
    return true;
}

/** Called only after the whole mutation has passed its revision checks under the State lock. */
inline void republish_members(SessionRecord& record,
                              std::size_t except = kInvalidMemberRow) noexcept {
    for (std::size_t row = 0; row < kInvalidMemberRow; ++row) {
        if (row == except) {
            continue;
        }
        auto* member = member_state(record, row);
        if (!member || !member->hasIdentity) {
            continue;
        }
        ++member->revision;
        member->acknowledgedRevision = membership::kAbsentRevision;
    }
}

/** Removes only the client's own state; the activity simulation and other members survive. */
inline void remove_joined_member(SessionRecord& record, std::size_t row) noexcept {
    if (!record.sharedMembers || !member_joined(record, row)) {
        return;
    }
    auto* member = member_state(record, row);
    *member = {};
    member->epoch = membership::session_epoch(record.createdRevision);
    if (row != 0) {
        record.coMembers[row - 1].joined = false;
    }
    entity_slots::depart_member_lease(record.memberLeases, row);
    record.heldEntitySlots = entity_slots::aggregate_member_leases(record.memberLeases);
    record.joined = record.memberLeases.joinedRows != 0;
}
} // namespace sunrise::state::activity
