#include "../../../../../state/activity/membership/member_directory.h"
#include "activity_peer_snapshot.h"

namespace sunrise::server::bap::encrypted::push {
void project_activity_peers(
    const state::activity::membership::MemberDirectory& directory,
    middleware::bap::activity_message::replicate_membership::MembershipSnapshot& output) noexcept {
    output.peers = {};
    if (!directory.valid) {
        return;
    }
    state::activity::reservations::Roster roster{};
    for (std::size_t i = 0; i < directory.peers.size(); ++i) {
        if (directory.peers[i].present) {
            roster.peers[i] = directory.peers[i].identity;
        }
    }
    project_activity_peers(roster, output);
    output.localSlot = directory.localSlot;
    for (std::size_t i = 0; i < directory.peers.size(); ++i) {
        const auto& source = directory.peers[i];
        auto& peer = output.peers[i];
        if (!source.present || !peer.present) {
            continue;
        }
        peer.slot = source.slot;
        peer.currentLeg = {source.currentLeg.sliceSetIndex,
                           source.currentLeg.hash,
                           source.currentLeg.index,
                           source.currentLeg.publicState,
                           source.currentLeg.auxState,
                           source.currentReported};
        peer.pendingLeg = {source.pendingLeg.sliceSetIndex,
                           source.pendingLeg.hash,
                           source.pendingLeg.index,
                           source.pendingLeg.publicState,
                           source.pendingLeg.auxState,
                           source.pendingReported};
        peer.hasTransitionToken = source.hasTransitionToken;
        peer.transitionToken = source.transitionToken;
        peer.hasSyncToken = source.hasTeleportToken;
        peer.syncToken = source.teleportToken;
    }
}
} // namespace sunrise::server::bap::encrypted::push
