#pragma once

namespace sunrise::middleware {

/** Initializes protocol middleware state. */
[[nodiscard]] bool initialize() noexcept;

/** Clears protocol middleware state. */
void shutdown() noexcept;

} // namespace sunrise::middleware
