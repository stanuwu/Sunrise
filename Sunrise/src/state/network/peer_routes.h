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
 * A host that goes silent without closing its socket is detected by the BAP link's own liveness
 * instead, so withdrawal there is bounded by the keepalive interval plus the response deadline
 * (`upstream_link.h`), not by a lease of this table's own. That is the one timer this
 * authorisation relies on.
 */
[[nodiscard]] bool replace(std::span<const Endpoint> endpoints) noexcept;
/** @return True while `endpoint` is in the currently accepted feed. */
[[nodiscard]] bool allows(Endpoint endpoint) noexcept;
/** Drops every accepted route until the next `replace`. */
void reset() noexcept;
} // namespace sunrise::state::network::peer_routes
