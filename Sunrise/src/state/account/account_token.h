#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::state::account {

/** The sign-on token field's own width, which both the request body and the reader declare. */
inline constexpr std::size_t kSignOnTokenSize = 32;

/** Its head carries the account SOID, so that many bytes are identity rather than key stream. */
inline constexpr std::size_t kSignOnTokenKeyBytes = 8;

void signon_token(std::uint64_t primarySoid, std::span<std::byte> output) noexcept;

[[nodiscard]] std::uint64_t soid_from_signon_token(std::span<const std::byte> token) noexcept;

} // namespace sunrise::state::account
