#pragma once

#include <cstddef>
#include <span>

namespace sunrise::middleware::bap::client_config {

/**
 * Encodes the minimal svc-19 response with its optional bytes omitted.
 * @param output Caller-owned response-body storage.
 * @param written Receives the exact encoded byte count, or zero on failure.
 * @return True when both required protobuf fields fit in the output.
 */
[[nodiscard]] bool encode_minimal_response(std::span<std::byte> output,
                                           std::size_t& written) noexcept;

} // namespace sunrise::middleware::bap::client_config
