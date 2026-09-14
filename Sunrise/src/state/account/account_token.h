#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::state::account {

inline constexpr std::size_t kSignOnTokenSize = 32;

inline constexpr std::size_t kSignOnTokenKeyBytes = 8;

void signon_token(std::uint64_t primarySoid, std::span<std::byte> output) noexcept;

[[nodiscard]] std::uint64_t soid_from_signon_token(std::span<const std::byte> token) noexcept;

} // namespace sunrise::state::account
