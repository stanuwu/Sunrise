#pragma once

#include <cstdint>

namespace sunrise::client::teleport {

/** Default distance, in world units along the camera's forward vector. */
inline constexpr float kDefaultDistance = 10.0F;
/** Smallest offered distance. Zero would leave the key bound to nothing visible. */
inline constexpr float kMinimumDistance = 1.0F;
/** Largest offered distance. Past this a press reliably lands through a wall or the floor. */
inline constexpr float kMaximumDistance = 100.0F;
/** No key is bound until one is picked, so a fresh install cannot teleport by accident. */
inline constexpr std::uint32_t kNoKey = 0;

/** Runtime teleport configuration. This module owns it; Core settings do not carry it. */
struct Settings {
    bool enabled{false};
    float distance{kDefaultDistance};
    std::uint32_t virtualKey{kNoKey};
};

/**
 * Resolves the configuration file and loads it when one exists.
 * @param module Loaded DLL used to resolve the owned artifact directory.
 */
void initialize(void* module) noexcept;

/** Drops the runtime configuration and the resolved file path. */
void shutdown() noexcept;

/** @return One lock-consistent copy of the current configuration. */
[[nodiscard]] Settings get() noexcept;

/**
 * Publishes one configuration and writes it straight to disk.
 * @param settings Candidate configuration, refused when a field is out of range.
 * @return True when the value was published. A failed write is logged, not returned.
 */
bool publish(const Settings& settings) noexcept;

} // namespace sunrise::client::teleport
