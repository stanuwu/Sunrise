#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::state::account {

/** The sign-on token field's own width, which both the request body and the reader declare. */
inline constexpr std::size_t kSignOnTokenSize = 32;

/** Its head carries the account SOID, so that many bytes are identity rather than key stream. */
inline constexpr std::size_t kSignOnTokenKeyBytes = 8;

/**
 * Derives the portable sign-on token for one account key. This is an identity marker, not
 * credential authentication.
 * @param output At least `kSignOnTokenSize` bytes. Left unchanged for a zero key or an
 * undersized output.
 */
void signon_token(std::uint64_t primarySoid, std::span<std::byte> output) noexcept;

/**
 * @return The account key this token derives from, confirmed by re-deriving the whole token
 * and comparing it rather than trusting the embedded head alone; zero for a wrong-sized,
 * zero-masked, or mismatched token.
 */
[[nodiscard]] std::uint64_t soid_from_signon_token(std::span<const std::byte> token) noexcept;

} // namespace sunrise::state::account
