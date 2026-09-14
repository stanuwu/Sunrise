#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "account_presence.h"

namespace sunrise::state::account::platform {

[[nodiscard]] constexpr bool valid(std::uint64_t id) noexcept {
    return (id >> 56U) == 1 && ((id >> 52U) & 0xFULL) == 1 && ((id >> 32U) & 0xFFFFFULL) == 1
           && (id & 0xFFFFFFFFULL) != 0;
}

[[nodiscard]] constexpr std::uint64_t declared_account_soid(std::uint64_t platformId) noexcept {
    std::uint64_t swapped = 0;
    for (std::size_t index = 0; index < sizeof platformId; ++index) {
        swapped = (swapped << 8U) | ((platformId >> (index * 8U)) & 0xFFULL);
    }
    return swapped << 8U;
}

[[nodiscard]] constexpr std::uint64_t
resolve(const AccountPresence& presence, bool local, std::uint64_t localPlatformId) noexcept {
    return presence.platformId != 0 ? presence.platformId : local ? localPlatformId : 0;
}

[[nodiscard]] constexpr std::array<std::byte, 36> identity_blob(std::uint64_t platformId) noexcept {
    std::array<std::byte, 36> output{};
    for (std::size_t index = 0; index < sizeof platformId; ++index) {
        output[index] = static_cast<std::byte>((platformId >> (index * 8U)) & 0xFFULL);
    }
    return output;
}

} // namespace sunrise::state::account::platform
