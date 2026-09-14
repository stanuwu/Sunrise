#pragma once
#include "../../core/network_capacity.h"
#include "../../middleware/gameplay/descriptor/net_addr.h"

namespace sunrise::state::network::peer_routes {
using Endpoint = middleware::gameplay::descriptor::PeerEndpoint;
inline constexpr std::size_t kCapacity =
    core::network_capacity::kPlayers * middleware::gameplay::descriptor::kPeerEndpointCount;
static_assert(kCapacity <= 255);
inline constexpr std::uint64_t kLeaseMs = 5'000;

// Replaces the snapshot after an accepted host feed or the playing host's local directory sync.
[[nodiscard]] bool replace(std::span<const Endpoint> endpoints, std::uint64_t now) noexcept;
[[nodiscard]] bool allows(Endpoint endpoint, std::uint64_t now) noexcept;
void reset() noexcept;
} // namespace sunrise::state::network::peer_routes
