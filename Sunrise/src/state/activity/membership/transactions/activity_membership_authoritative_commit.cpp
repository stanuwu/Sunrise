#include "internal.h"

namespace sunrise::state::activity::membership::transactions {

/** Merges one sparse client report into host-owned State; it grants no mission authority. */
bool commit_authoritative(ActivityState& state,
                          SessionRecord& record,
                          const PendingMutation& prepared,
                          CommittedClientState& clientState) noexcept {
    clientState = {};
    auto& member = *member_state(record, prepared.memberRow);
    if (!equal(prepared.authoritativeInput, prepared.authoritativeGuard)) {
        return false;
    }

    // The merge decides the outcome. The prepared plan is not compared against it. State moves
    // between prepare and commit, and refusing on that difference dropped the whole delta: no
    // revision advanced and the region never moved.
    const MembershipState before = member;
    clientState.previousRegion = before.currentRegion.index;
    MembershipState merged = merge(before, prepared.authoritativeInput);
    const bool changed = !equal_authoritative(member, merged);
    const bool movesRegion = moves_region(member, merged);
    const bool revisionExhausted =
        state.stateRevision == activity::kMaximumRevision
        || (member.hasIdentity && member.revision == kMaximumMembershipRevision);
    if ((changed || movesRegion) && revisionExhausted) {
        return false;
    }
    if (!changed && !movesRegion) {
        clientState.activityStateRevision = state.stateRevision;
        clientState.membershipRevision = member.revision;
        clientState.heldRegion = member.currentRegion.index;
        clientState.committed = true;
        return true;
    }

    // Only a changed published field earns a revision. A bare region move changes nothing the
    // membership body carries; a public region's new advertisement is republished by the keepalive
    // when the region it advertises changes.
    if (changed && member.hasIdentity) {
        ++merged.revision;
        merged.acknowledgedRevision = kAbsentRevision;
    }
    // The host teleport's spawn step and its retirement are part of the merge, so the staged
    // answer to the report already carries them.
    member = merged;
    publish_change(state, record);
    clientState.region = member.region;
    clientState.currentRegion = member.currentRegion;
    clientState.heldRegion = member.currentRegion.index;
    clientState.activityStateRevision = state.stateRevision;
    clientState.membershipRevision = member.revision;
    clientState.teleportSliceSetIndex = member.teleport.sliceSetIndex;
    clientState.teleportSliceSetHash = member.teleport.spawnSetHash;
    clientState.spawnState = member.spawn.state;
    clientState.teleportState = member.teleport.state;
    clientState.hasRegion = movesRegion && member.region.index >= 0;
    clientState.hasCurrentRegion =
        before.currentRegion.index != member.currentRegion.index && member.currentRegion.index >= 0;
    clientState.hasSpawn = before.spawn.state != member.spawn.state;
    clientState.hasTeleport = before.teleport.state != member.teleport.state
                              || before.teleport.sliceSetIndex != member.teleport.sliceSetIndex
                              || before.teleport.spawnSetHash != member.teleport.spawnSetHash;
    clientState.changed = clientState.hasRegion || clientState.hasCurrentRegion
                          || clientState.hasSpawn || clientState.hasTeleport;
    clientState.committed = true;
    return true;
}

} // namespace sunrise::state::activity::membership::transactions
