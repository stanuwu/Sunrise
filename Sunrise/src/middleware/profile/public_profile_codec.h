#pragma once

#include <cstddef>
#include <span>

#include "../../state/account/account_state.h"

namespace sunrise::middleware::profile {

inline constexpr std::size_t kMaximumEncodedSize = 256 * 1024;

/** The internal profile exchange carries identity and character content, never account preferences.
 */
[[nodiscard]] bool encode(const state::AccountState& account,
                          std::span<std::byte> output,
                          std::size_t& written) noexcept;
/** Refuses unsupported versions, malformed fields and trailing bytes without changing output. */
[[nodiscard]] bool decode(std::span<const std::byte> input, state::AccountState& output) noexcept;
[[nodiscard]] bool valid(const state::AccountState& account) noexcept;

} // namespace sunrise::middleware::profile
