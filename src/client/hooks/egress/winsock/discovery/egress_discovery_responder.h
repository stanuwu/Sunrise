#pragma once

#include <cstddef>
#include <span>

#include "../../platform/sdk.h"

namespace sunrise::client::hooks::egress::winsock::discovery {

/** The original Winsock send entry, used to skip the installed detour. */
using SendTo = decltype(&::sendto);

/** Discovery handling decision and replacement send result. */
struct Result final {
    bool handled{};
    int result{};
};

/**
 * Handles one verified discovery request without external network access.
 * @return Whether the request was handled and its replacement send result.
 */
[[nodiscard]] Result handle(SOCKET socket,
                            std::span<const std::byte> payload,
                            int flags,
                            const sockaddr* destination,
                            int destinationLength,
                            SendTo sendTo) noexcept;

} // namespace sunrise::client::hooks::egress::winsock::discovery
