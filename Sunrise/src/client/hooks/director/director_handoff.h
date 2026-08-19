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

} // namespace sunrise::client::hooks::director
