#pragma once

#include <cstddef>
#include <cstdint>

#include "account_presence.h"

namespace sunrise::state::account::platform {

/** A platform id's four fields: public universe, individual account type, desktop instance, id. */
[[nodiscard]] constexpr bool valid(std::uint64_t id) noexcept {
    return (id >> 56U) == 1 && ((id >> 52U) & 0xFULL) == 1 && ((id >> 32U) & 0xFFFFFULL) == 1
           && (id & 0xFFFFFFFFULL) != 0;
}

/**
 * The account SOID one platform id declares: that id byte-reversed, then raised one byte.
 * Raising it drops the platform id's own lowest byte, which every generated id holds fixed, and
 * leaves the SOID's lowest byte clear. The investment store's identity bind requires that form.
 */
[[nodiscard]] constexpr std::uint64_t declared_account_soid(std::uint64_t platformId) noexcept {
    std::uint64_t swapped = 0;
    for (std::size_t index = 0; index < sizeof platformId; ++index) {
        swapped = (swapped << 8U) | ((platformId >> (index * 8U)) & 0xFFULL);
    }
    return swapped << 8U;
}

/**
 * @return The presence's own platform id, or the local installation's id for the local
 * player, else zero.
 */
[[nodiscard]] constexpr std::uint64_t
resolve(const AccountPresence& presence, bool local, std::uint64_t localPlatformId) noexcept {
    return presence.platformId != 0 ? presence.platformId : local ? localPlatformId : 0;
}

} // namespace sunrise::state::account::platform
