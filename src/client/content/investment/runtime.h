#pragma once

namespace sunrise::client::content::investment {

/**
 * Publishes installed mappings and the process-only selected-character light scalar.
 * @return True when every mapping is durable and the current scalar is ready when needed.
 */
[[nodiscard]] bool refresh() noexcept;

} // namespace sunrise::client::content::investment
