#pragma once

#include <cstdint>

namespace sunrise::client::hooks::membership_probe {

/**
 * Attaches the read-only probe on the client's activity msg 12 handler.
 * It answers one question: does a membership body we push on a public-target link reach the
 * client at all, and what identity does that link's ActivityClient hold. Nothing is changed --
 * the detour logs and calls through.
 * @return True when the probe is attached, or was already.
 */
[[nodiscard]] bool install() noexcept;

/**
 * Samples every ActivityClient the probe has seen recently.
 * The container bind happens on a later tick than the message, so the message handler is too
 * early to see its effect. Sampling stops a bounded time after the last message on a client, so
 * a torn-down activity is not read through a stale pointer.
 * @param now Monotonic tick count in milliseconds.
 */
void service(std::uint64_t now) noexcept;

/** Detaches the probe so a later unload cannot leave a detour into unmapped code. */
[[nodiscard]] bool uninstall() noexcept;

} // namespace sunrise::client::hooks::membership_probe
