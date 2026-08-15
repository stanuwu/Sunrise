#pragma once

#include <cstddef>
#include <span>

#include "../../../state/runtime/state.h"

namespace sunrise::middleware::signon::config {

/** Magic, key, one length byte, the token, and the null. */
inline constexpr std::size_t kBlobSize = 13 + state::kBootstrapTokenSize;

/**
 * Builds the bootstrap config blob around the native token.
 * @param token Exactly 16 native bytes.
 * @param output Blob storage, exactly kBlobSize bytes.
 */
void build(std::span<const std::byte> token, std::span<std::byte> output) noexcept;

} // namespace sunrise::middleware::signon::config
