#pragma once

namespace sunrise::izanami::runtime::baseplate_composition {

/** Prepares Izanami's isolated native-world composition for the next destination arrival. */
[[nodiscard]] bool arm() noexcept;

/** Cancels an armed composition and forgets arrival-local state. */
void disarm() noexcept;

/** Relocates the player and places the native baseplate. Call from a game-thread frame hook. */
void poll() noexcept;

} // namespace sunrise::izanami::runtime::baseplate_composition
