#pragma once

namespace sunrise::client::hooks::director {

struct HandoffResult {
    bool requested{};
    bool usedDestinationsTab{};
    bool usedDirector{};
    bool usedFallback{};
    bool accountBindingsConfigured{};
};

struct ActivityLaunchResult {
    bool requested{};
    bool targetResolved{};
    bool inOrbit{};
};

/**
 * Installs the one-shot local activity-carrier probe during the main-image hook sweep.
 * A miss is diagnostic and must not prevent ordinary Sunrise activation.
 */
[[nodiscard]] bool install_local_carrier() noexcept;

/**
 * Queues Destiny's native orbit-to-activity transition for the next game-thread poll.
 * @return Resolution and orbit validation details for the request.
 */
[[nodiscard]] ActivityLaunchResult request_activity_launch() noexcept;

/**
 * Requests a short native key pulse that opens Destiny's Director.
 * The pulse is emitted on the next game-frame polls so the game, not Sunrise UI code, owns the
 * transition into its Destinations experience.
 */
[[nodiscard]] HandoffResult request_open_destinations() noexcept;

/** Runs a queued activity transition or Director key pulse. Call from a game-thread hook. */
void poll() noexcept;

/** Cancels pending launch work and releases the spoofed key. */
void cancel() noexcept;

/** Cancels pending work and detaches the local-selection hook before client teardown. */
void shutdown() noexcept;

} // namespace sunrise::client::hooks::director
