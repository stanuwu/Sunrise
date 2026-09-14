#pragma once

#include <span>

#include "native_presence.h"

namespace sunrise::state::social {

struct NativePublication {
    std::uint64_t primarySoid{};
    NativePresence presence{};
};

/** Relays the requested account's native roster record without projecting membership or UI state.
 */
[[nodiscard]] bool project_fireteam(std::uint64_t accountSoid,
                                    std::span<const NativePublication> publications,
                                    std::array<std::byte, kNativeFireteamSize>& output) noexcept;

} // namespace sunrise::state::social
