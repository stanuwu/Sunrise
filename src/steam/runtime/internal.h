#pragma once

namespace sunrise::steam::runtime {

/**
 * Activates the main-image Client group at the first Core-ready boundary.
 * @return Stored result of the one activation that ran, or false while Core is not ready.
 */
[[nodiscard]] bool activate_main_once() noexcept;

/** @return True while the main-image sweep has not run yet. */
[[nodiscard]] bool main_activation_pending() noexcept;

/** Installs the presentation hooks once, from the callback pump. */
void activate_graphics_once() noexcept;

/**
 * Activates the platform Client group at its exact interface request boundary.
 * @param callerAddress Return address caught by the exported interface factory.
 */
void activate_platform_once(const void* callerAddress) noexcept;

} // namespace sunrise::steam::runtime
