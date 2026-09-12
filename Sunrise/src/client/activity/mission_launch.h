#pragma once
#include <cstdint>

#include "../../state/activity/forced/definition.h"
namespace sunrise::client::activity::mission_launch {
enum class Status : std::uint8_t {
    idle,
    requested,
    queued,
    arrived,
    catalogUnavailable,
    entryUnavailable,
    overrideActive,
    returnToOrbit,
    nativeUnavailable,
    notReady,
    descriptorRejected,
    timedOut,
    manualRejected
};
struct Snapshot {
    Status status{Status::idle};
    std::uint16_t index{};
    bool busy{};
    bool manual{};
    state::activity::forced::ForcedDestination destination{};
};
/** Finds the Director's selection entry points. @return True when every one was found. */
[[nodiscard]] bool install() noexcept;
/** Forgets the entry points, so a pending request fails instead of calling into a gone image. */
void uninstall() noexcept;
/** Render thread: enqueue one immutable public activity ordinal. No native calls here. */
[[nodiscard]] bool request(std::uint16_t index) noexcept;
/** Enqueues a copied draft; the game-frame owner validates and publishes it before launch. */
[[nodiscard]] bool
request_manual(std::uint16_t index,
               const state::activity::forced::ForcedDestination& destination) noexcept;
/** @return A synchronized copy of the latest request and its native transition status. */
[[nodiscard]] Snapshot snapshot() noexcept;
/**
 * Advances the pending request. Game thread only, once per frame.
 * @param step The client's current boot-flow step.
 */
void poll(std::int32_t step) noexcept;
/** @return A static UI message explaining the request result or next required action. */
[[nodiscard]] const char* description(Status status) noexcept;
} // namespace sunrise::client::activity::mission_launch
