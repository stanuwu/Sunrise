#pragma once

namespace sunrise::client::input {

/**
 * The asynchronous key state reports a key held whichever application owns it. Every movement key
 * poll is gated on this, because the game stops acting on input when it loses focus.
 * @return True while a window of this process is in front.
 */
[[nodiscard]] bool game_focused() noexcept;

} // namespace sunrise::client::input
