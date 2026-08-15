#pragma once

#include <Windows.h>

namespace sunrise::client {

/**
 * Initializes Client-owned process state without installing hooks.
 * @param module Loaded DLL module used for module-relative Client configuration.
 */
[[nodiscard]] bool initialize(void* module) noexcept;

/** Resolves main-image targets, installs game hooks, and attempts optional graphics hooks once. */
[[nodiscard]] bool activate_main_once() noexcept;

/** Installs the presentation hooks once, independently of the game image sweep. */
[[nodiscard]] bool activate_graphics_once() noexcept;

/**
 * Resolves targets in one loaded Steam networking image and installs its hook group once.
 * @param networkingModule Exact loaded steamnetworkingsockets module.
 * @return True when the platform stage is active or was already active.
 */
[[nodiscard]] bool activate_platform_once(HMODULE networkingModule) noexcept;

/** Removes Client hooks and clears resolved process targets. */
[[nodiscard]] bool shutdown() noexcept;

} // namespace sunrise::client
