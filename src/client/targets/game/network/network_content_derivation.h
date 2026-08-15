#pragma once

#include <span>

#include "../network.h"

namespace sunrise::client::targets::game::network::content_derivation {

/**
 * Derives the content-network targets without publishing them.
 * @param image Executable ranges from the main game image.
 * @param fetch Matched config fetch wrapper.
 * @param gate Matched signature-gate instruction sequence.
 * @param resolved Receives staged addresses; callers must discard it after failure.
 * @return True when both instruction shapes and targets remain valid.
 */
[[nodiscard]] bool derive(std::span<const patterns::ImageRange> image,
                          std::byte* fetch,
                          std::byte* gate,
                          Targets& resolved) noexcept;

} // namespace sunrise::client::targets::game::network::content_derivation
