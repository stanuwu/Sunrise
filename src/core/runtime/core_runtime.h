#pragma once

namespace sunrise::core {

/** Initializes configuration, logging, and all runtime layers. */
[[nodiscard]] bool initialize(void* module) noexcept;

/** Shuts down runtime layers in reverse dependency order. */
[[nodiscard]] bool shutdown() noexcept;

/** Reports whether Core finished initialization. */
[[nodiscard]] bool is_initialized() noexcept;

} // namespace sunrise::core
