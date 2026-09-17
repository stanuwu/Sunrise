#pragma once
#include "../../core/network_capacity.h"
#include "../../middleware/gameplay/descriptor/net_addr.h"

namespace sunrise::state::network::peer_routes {
/** One accepted peer's routable address. */
using Endpoint = middleware::gameplay::descriptor::PeerEndpoint;
/** Every player's worth of endpoints the accepted feed can authorise at once. */
inline constexpr std::size_t kCapacity =
    core::network_capacity::kPlayers * middleware::gameplay::descriptor::kPeerEndpointCount;
/** The feed writes the route count as one byte, so every route must be expressible. */
static_assert(kCapacity <= 255);

/**
 * Endpoints the accepted social feed currently authorises, withdrawn by lifecycle rather than by
 * age: an emptier feed, a peer's last link closing, a withdrawn native presence, an account
 * release, or the registered social link itself closing.
 * Guests detect a silent host through the upstream BAP keepalive and response deadline
 * (`upstream_link.h`), paused while local output is blocked. Hosts enable TCP keepalive on every
 * accepted connection: 30 seconds idle plus ten one-second unanswered probes on Windows.
 * Transport cleanup withdraws a lost peer's authorization; healthy idle peers retain it.
 */
[[nodiscard]] bool replace(std::span<const Endpoint> endpoints) noexcept;
/** @return True while `endpoint` is in the currently accepted feed. */
[[nodiscard]] bool allows(Endpoint endpoint) noexcept;
/** Drops every accepted route until the next `replace`. */
void reset() noexcept;
} // namespace sunrise::state::network::peer_routes
