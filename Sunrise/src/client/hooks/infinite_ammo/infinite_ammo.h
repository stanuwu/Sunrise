#pragma once

namespace sunrise::client::hooks::infinite_ammo {

/**
 * Attaches to the reserve, magazine and sword setters. Infinite Magazine sends a fixed magazine
 * amount while weapons still use the existing setter path.
 * @return True when all three resolved and the detours attached.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches every detour. */
void uninstall() noexcept;

} // namespace sunrise::client::hooks::infinite_ammo
