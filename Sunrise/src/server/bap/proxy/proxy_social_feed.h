#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::server::bap::proxy::social_feed {
void reset() noexcept;
void abandon(std::uint32_t connection, std::uint32_t task) noexcept;
[[nodiscard]] bool
acknowledge(std::uint32_t connection, std::uint32_t task, std::span<const std::byte> body) noexcept;
/** Absorbs one host publication notice. Internal: it never enters the downstream reply queue. */
[[nodiscard]] bool notify(std::uint32_t connection, std::span<const std::byte> payload) noexcept;
/** Withdraws the authorisation this link carried, even when another link survives. */
void connection_closed(std::uint32_t connection) noexcept;
/** Level-triggered: every term is read from current state, so a refusal simply retries. */
void service() noexcept;
} // namespace sunrise::server::bap::proxy::social_feed
