#pragma once

#include <cstddef>
#include <span>

#include "../../../state/entitlements/definition.h"

namespace sunrise::middleware::signon::ownership {

/** Each owned id takes at most 1 key byte and 5 varint bytes. */
inline constexpr std::size_t kEntryBytes = 6;
/** Scratch size covers a full authored ownership table. */
inline constexpr std::size_t kBufferSize = state::entitlements::kCapacity * kEntryBytes;

/**
 * Encodes the repeated ownership list the Client matches against the content manifest.
 * Definitions that are not owned are left out on purpose.
 * @param entitlements Validated authored ownership policy.
 * @param size Cleared, then receives the encoded byte count.
 * @return True when every owned id fits.
 */
[[nodiscard]] bool encode(const state::entitlements::Table& entitlements,
                          std::span<std::byte> output,
                          std::size_t& size) noexcept;

} // namespace sunrise::middleware::signon::ownership
