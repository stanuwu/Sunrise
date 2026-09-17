#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::server::bap::proxy::social_feed {
/** Clears the registration and withdraws this feed's route authorisation. */
void reset() noexcept;
/** Clears the outstanding task only if it is still `task` on `connection`; otherwise a no-op. */
void abandon(std::uint32_t connection, std::uint32_t task) noexcept;
/**
 * Applies one accepted feed answer: replaces the peer route table and advances the publication
 * high-water mark. False on a decode or apply failure leaves the outstanding task and routes
 * untouched.
 */
[[nodiscard]] bool
acknowledge(std::uint32_t connection, std::uint32_t task, std::span<const std::byte> body) noexcept;
/** Absorbs an internal host notice without downstream delivery. A malformed notice fails its
 * registered connection; false is terminal here, not temporary output pressure. */
[[nodiscard]] bool notify(std::uint32_t connection, std::span<const std::byte> payload) noexcept;
/** Withdraws the authorisation this link carried, even when another link survives. */
void connection_closed(std::uint32_t connection) noexcept;
/** Level-triggered: every term is read from current state, so a refusal simply retries. */
void service() noexcept;
} // namespace sunrise::server::bap::proxy::social_feed
