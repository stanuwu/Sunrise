#pragma once

#include <Windows.h>

namespace sunrise::core::ui::modules::logs {

/** @return True when the Core Logs page owns its registry slot. */
[[nodiscard]] bool initialize() noexcept;

/** Removes the Core Logs page and clears its local filter state. */
void shutdown() noexcept;

/**
 * Dispatches one user-requested clipboard copy after presentation locks unwind.
 * @param owner Active game output window that becomes the clipboard owner.
 */
void dispatch_pending_copy(HWND owner) noexcept;

} // namespace sunrise::core::ui::modules::logs
