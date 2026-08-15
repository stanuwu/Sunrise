#pragma once

#include <Windows.h>

#include <string_view>

#include "../../../logging/view/log_snapshot_view.h"

namespace sunrise::core::ui::modules::logs::internal {

/** Draws the Logs page inside the active Core UI frame. */
void draw() noexcept;

/** Clears local filter and copy-result state between UI lifecycles. */
void reset() noexcept;

/** State of the explicit clipboard action. */
enum class CopyStatus : unsigned char {
    idle,
    pending,
    copied,
    failed,
};

/**
 * Checks whether one status label fits in the space left after the copy button.
 * @param availableWidth Width left on the current row in framebuffer pixels.
 * @param spacing Required gap before the status label in framebuffer pixels.
 * @param statusWidth Drawn status-label width in framebuffer pixels.
 * @return True when the gap and the whole label fit on the current row.
 */
[[nodiscard]] constexpr bool
copy_status_fits_inline(float availableWidth, float spacing, float statusWidth) noexcept {
    return availableWidth >= spacing + statusWidth;
}

/** Clipboard bridge supplied by the Win32 dispatch or by a test. */
using CopyAction = bool (*)(HWND owner, std::string_view payload) noexcept;

/**
 * Queues all visible structured log lines without calling Win32 from the render lock.
 * @param visible Borrowed selection whose source snapshot stays alive for the call.
 * @return True when this payload becomes the one pending copy request.
 */
[[nodiscard]] bool request_copy(const log::view::Result& visible) noexcept;

/** @return State of the last explicit clipboard request, read under the lock. */
[[nodiscard]] CopyStatus copy_status() noexcept;

/**
 * Runs one pending payload through a caller-supplied bridge, after render locks unwind.
 * @param owner Valid game or test window that becomes the Win32 clipboard owner.
 * @param action Non-null clipboard bridge.
 * @return The bridge result, or false when no valid request can be sent.
 */
[[nodiscard]] bool dispatch_pending_copy(HWND owner, CopyAction action) noexcept;

/** Runs one pending payload through the Win32 clipboard bridge. */
void dispatch_pending_copy(HWND owner) noexcept;

/** Cancels pending work and clears the kept clipboard bytes at a UI lifecycle boundary. */
void cancel_pending_copy() noexcept;

} // namespace sunrise::core::ui::modules::logs::internal
