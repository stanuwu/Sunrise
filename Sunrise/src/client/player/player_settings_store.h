#pragma once

namespace sunrise::client::player {

/** Runtime player configuration. This module owns it; Core settings do not carry it. */
struct Settings {
    bool infiniteAmmoEnabled{false};
    bool noTurnbackEnabled{false};
    bool godmodeEnabled{false};
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
 * @param settings Configuration to store.
 * @return True when the value was published. A failed write is logged, not returned.
 */
bool publish(const Settings& settings) noexcept;

} // namespace sunrise::client::player
