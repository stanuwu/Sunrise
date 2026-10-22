#pragma once

namespace sunrise::client::hooks::presence_publication {
/**
 * Publishes changed native fireteam rows after their builder finishes. Row-only changes do not
 * arm the native urgent publisher, leaving member cards stale until its periodic publication.
 * Calls the native publisher without constructing or changing membership rows.
 * @return True when the uniquely matched builder is attached.
 */
[[nodiscard]] bool install() noexcept;
/** @return True after detaching; false retains ownership for a later retry. */
[[nodiscard]] bool uninstall() noexcept;
} // namespace sunrise::client::hooks::presence_publication
