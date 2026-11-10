#pragma once

#include <span>

#include "native_presence.h"

namespace sunrise::state::social {

/** One roster entry `project_fireteam` searches by account. */
struct NativePublication {
    std::uint64_t primarySoid{};
    NativePresence presence{};
};

/** Copies the requested account's published native roster record, tail included. */
[[nodiscard]] bool project_fireteam(std::uint64_t accountSoid,
                                    std::span<const NativePublication> publications,
                                    std::array<std::byte, kNativeFireteamSize>& output) noexcept;

} // namespace sunrise::state::social
