#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::server::bap::proxy::social_feed {
void reset() noexcept;
void abandon(std::uint32_t connection, std::uint32_t task) noexcept;
[[nodiscard]] bool
acknowledge(std::uint32_t connection, std::uint32_t task, std::span<const std::byte> body) noexcept;
void service(std::uint64_t now) noexcept;
} // namespace sunrise::server::bap::proxy::social_feed
