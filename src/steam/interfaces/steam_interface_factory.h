#pragma once

namespace sunrise::steam::interfaces {

/**
 * Creates one supported Steam client interface.
 * @return Interface object, or null when the version is not supported.
 */
[[nodiscard]] void* create(const char* version) noexcept;

/**
 * Returns one supported interface for the active Steam user.
 * @return Interface object, or null when the version is not supported.
 */
[[nodiscard]] void* find_user(const char* version) noexcept;

/**
 * Returns one supported process-global interface.
 * @return The utils interface, or null for any other version.
 */
[[nodiscard]] void* find_global(const char* version) noexcept;

} // namespace sunrise::steam::interfaces
