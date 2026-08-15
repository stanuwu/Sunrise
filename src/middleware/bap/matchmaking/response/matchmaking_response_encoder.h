#pragma once

#include <cstddef>
#include <span>

#include "../definition.h"

namespace sunrise::middleware::bap::matchmaking::response {

/**
 * Encodes one service-43 body. The service-42 request kind picks the shape.
 * @param response Checked response fields and an optional borrowed descriptor.
 * @param output Caller-owned body storage, left unchanged on failure.
 * @param written Receives the whole encoded body size or zero on failure.
 * @return True when the chosen empty, static, or dynamic response fits.
 */
[[nodiscard]] bool
encode(const Response& response, std::span<std::byte> output, std::size_t& written) noexcept;

} // namespace sunrise::middleware::bap::matchmaking::response
