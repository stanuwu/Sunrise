#pragma once

#include "../../../state/entitlements/definition.h"

namespace sunrise::core::settings::server {

/** The loopback port the BAP listener binds, and the relay port SignOn hands the Client. */
inline constexpr std::uint16_t kDefaultBapPort = 30974;

/** Read-only Server settings. */
struct Settings {
    /** Authored ownership policy declared by SignOn and defined by the content manifest. */
    state::entitlements::Table entitlements{};
    /** BAP port. The listener binds it and SignOn publishes it. Zero is the no-relay sentinel. */
    std::uint16_t bapPort{kDefaultBapPort};
};

} // namespace sunrise::core::settings::server
