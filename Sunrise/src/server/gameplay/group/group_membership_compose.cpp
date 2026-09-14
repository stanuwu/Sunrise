#include "group_membership_compose.h"

namespace sunrise::server::gameplay::group::compose {
bool membership(
    std::uint64_t groupSessionId,
    const std::array<std::byte, middleware::gameplay::descriptor::kNetAddrSize>& hostAddress,
    std::span<const PeerInput> peers,
    std::uint32_t revision,
    Membership& output) noexcept {
    namespace wire = middleware::gameplay::group;
    if (!groupSessionId || !revision || peers.empty() || peers.size() > kPeerCapacity) {
        return false;
    }
    const PeerInput* recipient{};
    for (std::size_t i = 0; i < peers.size(); ++i) {
        const auto& peer = peers[i];
        if (!peer.member.machineId || peer.member.machineId == groupSessionId
            || (peer.hasPlayer
                && (!peer.player.playerId || peer.player.slot >= wire::kPlayerCapacity))) {
            return false;
        }
        if (peer.recipient) {
            if (recipient) {
                return false;
            }
            recipient = &peer;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (peers[j].member.machineId == peer.member.machineId) {
                return false;
            }
            if (peer.hasPlayer && peers[j].hasPlayer
                && (peers[j].player.slot == peer.player.slot
                    || peers[j].player.playerId == peer.player.playerId
                    || (peer.player.hasProfile && peers[j].player.hasProfile
                        && peers[j].player.accountSoid == peer.player.accountSoid))) {
                return false;
            }
        }
    }
    if (!recipient) {
        return false;
    }
    output = {};
    auto& host = output.members[0];
    host.address = hostAddress;
    host.machineId = groupSessionId;
    host.joinId = recipient->member.joinId;
    host.state = recipient->member.state;
    host.connectionPresent = true;
    std::size_t playerCount{};
    for (std::size_t i = 0; i < peers.size(); ++i) {
        auto& member = output.members[i + 1];
        member = peers[i].member;
        member.ownsPlayerSlot = peers[i].hasPlayer;
        if (peers[i].hasPlayer) {
            member.playerSlot = peers[i].player.slot;
            auto& player = output.players[playerCount++];
            player = peers[i].player;
            player.memberIndex = static_cast<std::uint32_t>(i + 1);
        }
    }
    output.update.hostMachineId = groupSessionId;
    output.update.revision = revision;
    output.update.hostMemberIndex = output.update.successionIndex = 0;
    output.update.members = std::span(output.members).first(peers.size() + 1);
    output.update.players = std::span(output.players).first(playerCount);
    return true;
}
} // namespace sunrise::server::gameplay::group::compose
