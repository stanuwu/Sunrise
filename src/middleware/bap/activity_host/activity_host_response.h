#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::activity_host {

/**
 * Checks one svc-16 host identity and encodes its svc-17 relay endpoint.
 * @param requestBody Complete fixed-width request body.
 * @param relayAddress Relay IPv4 address in host order. Must not be zero.
 * @param relayPort Relay port in host order. Must not be zero.
 * @param output Caller-owned response-body storage, unchanged on failure.
 * @param written Receives the response size, or zero on failure.
 * @return True when the request and the relay endpoint are valid and fit.
 */
[[nodiscard]] bool encode_response(std::span<const std::byte> requestBody,
                                   std::uint32_t relayAddress,
                                   std::uint16_t relayPort,
                                   std::span<std::byte> output,
                                   std::size_t& written) noexcept;

} // namespace sunrise::middleware::bap::activity_host
