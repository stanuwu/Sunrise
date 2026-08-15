#pragma once

#include <cstddef>
#include <span>

#include "../../../state/account/account_state.h"

namespace sunrise::middleware::datagen::family3 {

/** The family-3 account roster occupies one 1,768-byte replicated object. */
inline constexpr std::size_t kRosterSize = 1768;

/**
 * Encodes an account roster from authored State identities.
 * @param account Account and character identities.
 * @param output Caller-owned raw object storage.
 * @param written Receives the exact roster object size.
 * @return True when State is valid and the complete object fits.
 */
[[nodiscard]] bool encode_roster(const state::AccountState& account,
                                 std::span<std::byte> output,
                                 std::size_t& written) noexcept;

} // namespace sunrise::middleware::datagen::family3
