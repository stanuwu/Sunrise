#pragma once

#include <array>
#include <span>

#include "../../../core/network_capacity.h"
#include "../../../middleware/gameplay/group/session_messages.h"

namespace sunrise::server::gameplay::group::compose {
/** One composed membership carries the whole activity, so it inherits that limit. */
inline constexpr std::size_t kPeerCapacity = core::network_capacity::kActivityPlayers;
/** One admitted peer's native row, and the player it asked this host to add. */
struct PeerInput final {
    middleware::gameplay::group::MembershipMember member{};
    middleware::gameplay::group::MembershipPlayer player{};
    bool hasPlayer{};
    bool recipient{};
};
/**
 * One composed snapshot. `update.members` and `update.players` are spans into `members` and
 * `players` below, so copying or moving a `Membership` leaves them pointing at the original.
 */
struct Membership final {
    std::array<middleware::gameplay::group::MembershipMember, kPeerCapacity + 1> members{};
    std::array<middleware::gameplay::group::MembershipPlayer, kPeerCapacity> players{};
    middleware::gameplay::group::MembershipUpdate update{};
};
/**
 * Composes one recipient's membership snapshot. Member slot 0 is always the host, followed by
 * `peers` in order. The caller supplies only admitted native rows and each player's own profile
 * fields. Peers without players remain members.
 * @param revision Publication revision; zero is refused.
 * @return False for invalid session/revision, invalid or duplicate peer/player identities,
 * capacity refusal, or anything other than one recipient. `output` is untouched on false.
 */
[[nodiscard]] bool
membership(std::uint64_t groupSessionId,
           const std::array<std::byte, middleware::gameplay::descriptor::kNetAddrSize>& hostAddress,
           std::span<const PeerInput> peers,
           std::uint32_t revision,
           Membership& output) noexcept;
} // namespace sunrise::server::gameplay::group::compose
