#include "internal.h"

namespace sunrise::state::activity::membership::transactions {

/** Merges and applies one sparse authoritative operation. */
bool commit_authoritative(ActivityState& state,
                          SessionRecord& record,
                          const PendingMutation& prepared) noexcept {
    if (!equal(prepared.authoritativeInput, prepared.authoritativeGuard)) {
        return false;
    }

    // The merge decides the outcome. The prepared plan is not compared against it. State moves
    // between prepare and commit, and refusing on that difference dropped the whole delta: no
    // revision advanced and the region never moved.
    MembershipState merged = merge(record.membership, prepared.authoritativeInput);
    const bool changed = !equal_authoritative(record.membership, merged);
    const bool revisionExhausted = state.stateRevision == activity::kMaximumRevision
                                   || (record.membership.hasIdentity
                                       && record.membership.revision == kMaximumMembershipRevision);
    if (changed && revisionExhausted) {
        return false;
    }
    const bool movesRegion = moves_region(record.membership, merged);
    if (!changed && !movesRegion) {
        return true;
    }

    if (changed && record.membership.hasIdentity) {
        ++merged.revision;
        merged.acknowledgedRevision = kAbsentRevision;
    }
    // The merged region is stored either way. A region-only move publishes no new membership, so
    // it must not bump the state revision the client checks its own table against.
    record.membership = merged;
    if (changed) {
        publish_change(state, record);
    }
    return true;
}

} // namespace sunrise::state::activity::membership::transactions
