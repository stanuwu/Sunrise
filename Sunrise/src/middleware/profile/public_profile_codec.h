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
/**
 * Decodes without allocation; rejection leaves output unchanged. The caller owns a distinct
 *
 * staging image, which may change on failure and must not overlap input. Aliased output/staging
 *
 * is refused. Separate staging permits concurrent decoders without a shared scratch lock.
 */
[[nodiscard]] bool decode(std::span<const std::byte> input,
                          state::AccountState& output,
                          state::AccountState& staging) noexcept;
[[nodiscard]] bool valid(const state::AccountState& account) noexcept;

} // namespace sunrise::middleware::profile
