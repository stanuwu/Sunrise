#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::activity_host_manager::response {

/**
 * Encodes a service-7 response with the minimal local activity-data policy. The zero
 * activity-data tail is needed here. Its protocol meaning is not known.
 * @param sessionId Activity-host session id. Must not be zero.
 * @param output Caller-owned response storage, unchanged on failure.
 * @param written Receives the response size, or zero on failure.
 * @return True when the session id is valid and the whole body fits.
 */
[[nodiscard]] bool encode_response(std::uint64_t sessionId,
                                   std::span<std::byte> output,
                                   std::size_t& written) noexcept;

} // namespace sunrise::middleware::bap::activity_host_manager::response
