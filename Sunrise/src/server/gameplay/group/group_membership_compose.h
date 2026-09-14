#pragma once

#include <array>
#include <span>

#include "../../../middleware/gameplay/group/session_messages.h"

namespace sunrise::server::gameplay::group::compose {
inline constexpr std::size_t kPeerCapacity = 12;
struct PeerInput final {
    middleware::gameplay::group::MembershipMember member{};
    middleware::gameplay::group::MembershipPlayer player{};
    bool hasPlayer{};
    bool recipient{};
};
struct Membership final {
    std::array<middleware::gameplay::group::MembershipMember, kPeerCapacity + 1> members{};
    std::array<middleware::gameplay::group::MembershipPlayer, kPeerCapacity> players{};
    middleware::gameplay::group::MembershipUpdate update{};
};
/** The caller supplies only admitted native rows and each player's own profile fields. */
[[nodiscard]] bool
membership(std::uint64_t groupSessionId,
           const std::array<std::byte, middleware::gameplay::descriptor::kNetAddrSize>& hostAddress,
           std::span<const PeerInput> peers,
           std::uint32_t revision,
           Membership& output) noexcept;
} // namespace sunrise::server::gameplay::group::compose
