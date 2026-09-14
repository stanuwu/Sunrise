#pragma once

#include "../../../../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../../../../state/activity/membership/member_directory.h"

namespace sunrise::server::bap::encrypted::push::activity {

/** Projects only this activity's joined, region-reporting members onto registered native slots. */
inline void
fill_member_roster(middleware::bap::activity_message::sensor_auth_update::Snapshot& snapshot,
                   const state::activity::membership::MemberDirectory& directory,
                   std::uint64_t localIdentity) noexcept {
    namespace message = middleware::bap::activity_message::sensor_auth_update;
    namespace scoreboard = middleware::bap::activity_message::scoreboard_record;
    snapshot.participationSeats = {};
    snapshot.participationSeatCount = 0;
    snapshot.perMemberParticipation = false;
    snapshot.scoreboard = {};
    snapshot.hasScoreboard = false;
    if (!directory.valid || localIdentity == 0 || snapshot.playerKey == 0
        || snapshot.roster.groupCount > snapshot.roster.groups.size()) {
        return;
    }
    std::array<std::uint64_t, scoreboard::kElementCount> keys{};
    keys[0] = localIdentity;
    std::size_t count = 1;
    for (const auto& peer : directory.peers) {
        if (!peer.present || !peer.joined || peer.identity.opaqueSoid == 0) {
            continue;
        }
        const bool sameRegion =
            snapshot.hasRegion
            && ((peer.currentReported && peer.currentLeg.index >= 0
                 && static_cast<std::uint32_t>(peer.currentLeg.index) == snapshot.region)
                || (peer.pendingReported && peer.pendingLeg.index >= 0
                    && static_cast<std::uint32_t>(peer.pendingLeg.index) == snapshot.region));
        if (!sameRegion) {
            continue;
        }
        bool duplicate = false;
        for (std::size_t prior = 0; prior < count; ++prior) {
            duplicate = duplicate || keys[prior] == peer.identity.opaqueSoid;
        }
        if (!duplicate && count < keys.size()) {
            keys[count++] = peer.identity.opaqueSoid;
        }
    }
    snapshot.perMemberParticipation = true;
    for (std::size_t group = 0; group < snapshot.roster.groupCount; ++group) {
        const message::Group& row = snapshot.roster.groups[group];
        if (row.retired || row.slotTypes.size() != row.slotIndices.size()
            || row.slotTypes.size() != row.slotFlags.size()) {
            continue;
        }
        for (std::size_t slot = 0; slot < row.slotTypes.size(); ++slot) {
            if ((row.slotFlags[slot] & message::kSlotAuthFlag) == 0) {
                continue;
            }
            snapshot.hasScoreboard =
                snapshot.hasScoreboard || row.slotTypes[slot] == scoreboard::kSlotType;
            if (row.key != snapshot.roster.playerKeyGroup || row.slotTypes[slot] != 13
                || snapshot.participationSeatCount >= count) {
                continue;
            }
            const std::size_t seat = snapshot.participationSeatCount++;
            snapshot.participationSeats[seat] = {seat == 0 ? snapshot.playerKey : keys[seat],
                                                 row.slotIndices[slot]};
        }
    }
    if (snapshot.hasScoreboard) {
        snapshot.scoreboard.elementCount = static_cast<std::uint8_t>(count);
        for (std::size_t seat = 0; seat < count; ++seat) {
            snapshot.scoreboard.elements[seat] = {keys[seat], true, true};
        }
    }
}

} // namespace sunrise::server::bap::encrypted::push::activity
