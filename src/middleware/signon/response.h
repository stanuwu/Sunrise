#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../state/entitlements/definition.h"
#include "../../state/runtime/state.h"

namespace sunrise::middleware::signon {

/** Encodes the required SignOn success fields into the caller buffer. */
[[nodiscard]] bool encode_success(const state::SignOnState& state,
                                  const state::entitlements::Table& entitlements,
                                  std::uint64_t expirySeconds,
                                  std::uint64_t serverTimeSeconds,
                                  std::span<std::byte> output,
                                  std::size_t& written) noexcept;

} // namespace sunrise::middleware::signon
