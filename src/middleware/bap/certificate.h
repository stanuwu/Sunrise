#pragma once

#include <cstddef>
#include <span>

namespace sunrise::middleware::bap::certificate {

/**
 * Rewraps a svc304 request certificate into the svc305 protobuf body.
 * @param requestBody Decrypted svc304 protobuf body.
 * @param output Caller-owned svc305 body storage.
 * @param written Receives encoded response-body bytes.
 * @return True when the response fits in caller storage.
 */
[[nodiscard]] bool encode_response(std::span<const std::byte> requestBody,
                                   std::span<std::byte> output,
                                   std::size_t& written) noexcept;

} // namespace sunrise::middleware::bap::certificate
