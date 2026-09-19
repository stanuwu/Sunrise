#pragma once

#include <array>

#include "../../../state/entitlements/definition.h"
#include "../../network_capacity.h"
#include "activation/definition.h"
#include "gameplay/definition.h"
#include "upstream/definition.h"

namespace sunrise::core::settings::server {

/** The loopback port the BAP listener binds, and the relay port SignOn hands the Client. */
inline constexpr std::uint16_t kDefaultBapPort = 30974;

/** Read-only Server settings. */
struct Settings {
    /** Maximum concurrently enrolled players; each may own several native connections. */
    std::size_t maxPlayers{network_capacity::kPlayers};
    /** Shared BAP interface. A playing host may bind all interfaces or loopback only. */
    std::array<unsigned char, 4> bapBind{127, 0, 0, 1};
    upstream::Settings upstream;
    /** Gameplay UDP endpoint topology. Disabled leaves the channel unpublished. */
    gameplay::Settings gameplay{};
    /** Per-domain gates for the default client-activation work. */
    activation::Settings activation{};
    /** BAP port. The listener binds it and SignOn publishes it. Zero is the no-relay sentinel. */
    std::uint16_t bapPort{kDefaultBapPort};
};

} // namespace sunrise::core::settings::server
