#pragma once

#include <Windows.h>

namespace sunrise::core::ui::busy::internal {

/** @return Mask of started tasks, one bit per Task. */
[[nodiscard]] unsigned running() noexcept;

/** @param threadId Thread that draws frames, which can never wait for its own. */
void set_present_thread(DWORD threadId) noexcept;

/**
 * Records that one frame drew the overlay.
 * @param complete True when it drew at full alpha, which is what a frozen screen must carry.
 */
void record_drawn(bool complete) noexcept;

} // namespace sunrise::core::ui::busy::internal
