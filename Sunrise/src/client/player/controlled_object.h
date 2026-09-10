#pragma once

#include <cstdint>

namespace sunrise::client::player::controlled_object {

/** Resolves the native accessor before its dependent hooks are installed. */
[[nodiscard]] bool resolve() noexcept;

/** Reports accessor availability, not whether a player currently exists. */
[[nodiscard]] bool available() noexcept;

/**
 * Reads the full handle of the object currently controlled by the local player.
 * Call only from a game thread that may invoke the native accessor.
 * @param handle Receives the full handle, or 0xFFFFFFFFU on failure.
 * @return True when the accessor returned a valid handle.
 */
[[nodiscard]] bool current_handle(std::uint32_t& handle) noexcept;

/** Clears the accessor only after every dependent hook has stopped. */
void clear() noexcept;

} // namespace sunrise::client::player::controlled_object