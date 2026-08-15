#pragma once

#include <array>

#include "network_hook_group.h"

namespace sunrise::client::hooks::network::lifecycle {

using GameSpecs = std::array<hooking::detour::Spec, kGameSlots.size()>;
using PlatformSpecs = std::array<hooking::detour::Spec, kPlatformSlots.size()>;
using GameProtectedEntries = std::array<hooking::detour::ProtectedCodeEntry, kGameSlots.size() + 2>;
using PlatformProtectedEntries =
    std::array<hooking::detour::ProtectedCodeEntry, kPlatformSlots.size() + 2>;

/** @return Game target and body pairs, in slot order. */
[[nodiscard]] GameSpecs game_specs() noexcept;

/** @return Platform target and body pairs, in slot order. */
[[nodiscard]] PlatformSpecs platform_specs() noexcept;

/** @return The game replacement, ingress and egress bodies kept safe at detach. */
[[nodiscard]] GameProtectedEntries game_protected_entries() noexcept;

/** @return The platform replacement, ingress and egress bodies kept safe at detach. */
[[nodiscard]] PlatformProtectedEntries platform_protected_entries() noexcept;

} // namespace sunrise::client::hooks::network::lifecycle
