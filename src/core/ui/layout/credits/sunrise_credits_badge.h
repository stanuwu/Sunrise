#pragma once

namespace sunrise::core::ui::layout::credits {

/** The badge label names both the action and where it goes. */
inline constexpr char kBadgeLabel[] = "Source / Credits";
/** Sunrise source and release credits live at this project URL. */
inline constexpr wchar_t kSourceUrl[] = L"https://github.com/stanuwu/Sunrise";

/** URL action from a user click. Supplied by the shell bridge or a test. */
using OpenUrlAction = bool (*)(const wchar_t* url) noexcept;

/** Queues one source action without leaving the active render call. */
void request_open() noexcept;

/** Cancels a queued source action at the layout lifecycle boundary. */
void cancel_pending() noexcept;

/**
 * Runs one queued source action through a caller-supplied bridge.
 * @param openUrl Non-null URL action.
 * @return The bridge result, or false when no request or action is available.
 */
[[nodiscard]] bool dispatch_pending(OpenUrlAction openUrl) noexcept;

/** Runs one queued source action through the Windows shell bridge. */
void dispatch_pending() noexcept;

/** Draws the full-width badge and queues the source URL only after a click. */
void draw() noexcept;

} // namespace sunrise::core::ui::layout::credits
