#pragma once

#include <cstdint>

namespace sunrise::core::settings {

/**
 * Layout version of the settings file this build writes and expects.
 * Raise it when a key is renamed, removed, changes meaning, or must take a new default.
 * Adding a key needs no raise, because a missing key already takes its default.
 */
inline constexpr std::uint32_t kSettingsVersion = 13;

} // namespace sunrise::core::settings
