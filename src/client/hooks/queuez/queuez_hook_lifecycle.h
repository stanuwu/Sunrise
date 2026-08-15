#pragma once

namespace sunrise::client::hooks::queuez {

/**
 * Attaches the family-zero source-list seed. The sweep declares one record per source key, so a
 * list the producer left empty leaves the client with no family-zero record, and no subscribe for
 * the object push to answer.
 * @return True when every fix attached.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches the family-zero source-list seed. */
void uninstall() noexcept;

/** @return True while at least one fix is attached. */
[[nodiscard]] bool is_installed() noexcept;

} // namespace sunrise::client::hooks::queuez
