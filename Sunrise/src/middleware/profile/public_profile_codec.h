#pragma once

#include <cstddef>
#include <span>

#include "../../state/account/account_state.h"

namespace sunrise::middleware::profile {

/**
 * Local profile-body budget; oversized encoding or input is refused. Version 6 needs at most
 * 8,015 bytes with three fully equipped characters and all native presence fields present.
 * The retained 256 KiB ceiling also budgets each of the publisher's two fixed staging buffers.
 */
inline constexpr std::size_t kMaximumEncodedSize = 256 * 1024;

/** The exchange carries identity and character content, never account preferences. */
[[nodiscard]] bool encode(const state::AccountState& account,
                          std::span<std::byte> output,
                          std::size_t& written) noexcept;
/**
 * Decodes without allocation; rejection leaves `output` unchanged. The caller owns a distinct
 * staging image, which may change on failure and must not overlap `input`; an aliased
 * output/staging pair is refused. Separate staging lets concurrent decoders run without
 * sharing a scratch lock.
 */
[[nodiscard]] bool decode(std::span<const std::byte> input,
                          state::AccountState& output,
                          state::AccountState& staging) noexcept;

/** @return True when `account` carries only what the public exchange may publish. */
[[nodiscard]] bool valid(const state::AccountState& account) noexcept;

} // namespace sunrise::middleware::profile
