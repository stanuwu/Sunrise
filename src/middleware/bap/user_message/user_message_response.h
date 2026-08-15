#pragma once

#include <cstddef>
#include <span>

namespace sunrise::middleware::bap::user_message {

/**
 * Encodes the minimal svc-33 response with all optional strings omitted.
 * @param output Caller-owned response-body storage.
 * @param written Receives the exact encoded byte count, or zero on failure.
 * @return True when the required protobuf field fits in the output.
 */
[[nodiscard]] bool encode_minimal_response(std::span<std::byte> output,
                                           std::size_t& written) noexcept;

} // namespace sunrise::middleware::bap::user_message
