#pragma once

#include "../member_selection.h"
#include "member_directory.h"

namespace sunrise::state::activity::membership {
/** Reservation slots remain stable; a recipient's own row is excluded from its peer collection. */
[[nodiscard]] inline MemberDirectory
member_directory(const SessionRecord& record,
                 const Identity& local,
                 const reservations::Roster* pendingPeers = nullptr) noexcept {
    MemberDirectory output{};
    const auto& roster = pendingPeers ? *pendingPeers : record.peerReservations;
    if (!local.memberKey || !local.accountSoid) {
        return output;
    }
    bool found = !record.joined || record.primaryIdentity.accountSoid == local.accountSoid;
    if (!found) {
        for (std::size_t i = 0; i < roster.peers.size(); ++i) {
            const auto& peer = roster.peers[i];
            if (peer.memberKey == local.memberKey && peer.accountSoid == local.accountSoid
                && peer.opaqueSoid == local.opaqueSoid) {
                output.localSlot = static_cast<std::uint8_t>(i + 2);
                found = true;
                break;
            }
        }
    }
    if (!found) {
        return output;
    }
    std::size_t next{};
    const auto add = [&](const Identity& identity, std::uint8_t slot) {
        if (!identity.memberKey || identity.accountSoid == local.accountSoid
            || next >= output.peers.size()) {
            return;
        }
        auto& peer = output.peers[next++];
        peer.identity = identity;
        peer.slot = slot;
        peer.present = true;
        const auto row = member_row(record, identity.memberKey, identity.accountSoid);
        if (const auto* member = member_state(record, row)) {
            peer.joined = true;
            if (member->hasIdentity) {
                peer.identity = member->identity;
            }
            peer.currentLeg = member->currentRegion;
            peer.pendingLeg = member->region;
            peer.currentReported = member->currentReported;
            peer.pendingReported = member->pendingReported;
            peer.hasTransitionToken = member->transitionReported;
            peer.hasTeleportToken = member->teleportReported;
            peer.transitionToken = member->nativeTransitionToken;
            peer.teleportToken = member->teleport.token;
        }
    };
    if (member_joined(record, 0)) {
        add(record.primaryIdentity, 0);
    } else {
        add(roster.primary, 0);
    }
    for (std::size_t i = 0; i < roster.peers.size(); ++i) {
        add(roster.peers[i], static_cast<std::uint8_t>(i + 2));
    }
    output.valid = true;
    return output;
}
} // namespace sunrise::state::activity::membership
