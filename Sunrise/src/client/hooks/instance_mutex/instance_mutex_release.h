#pragma once

namespace sunrise::client::hooks::instance_mutex {
/**
 * Resolves the native wait import by unique signature and arranges release on its owner
 * thread: the startup pacing loop that holds the single-instance mutexes calls this same
 * import from the thread that owns them.
 */
[[nodiscard]] bool install() noexcept;
/** @return True after releasing import ownership; false retains it for a later retry. */
[[nodiscard]] bool uninstall() noexcept;
/** Attempts the single-instance release on this thread without waiting for another owner. */
void release_once() noexcept;
} // namespace sunrise::client::hooks::instance_mutex
