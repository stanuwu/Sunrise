#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::matchmaking::response {

/**
 * Encodes kind 2 or 5 around one nested advertisement id.
 * @param includeAssignmentResult True only when kind 2 needs field 1 value 2.
 * @param advertisementId Nonzero id returned to the client.
 * @param output Caller-owned response storage, left unchanged on failure.
 * @param written Receives the whole response size or zero on failure.
 * @return True when the id is valid and the response fits.
 */
[[nodiscard]] bool encode_advertisement_id(bool includeAssignmentResult,
                                           std::uint64_t advertisementId,
                                           std::span<std::byte> output,
                                           std::size_t& written) noexcept;

/**
 * Encodes one locate-session result with its id and descriptor.
 * @param advertisementId Nonzero id of the latest advertisement.
 * @param descriptor Exactly 128 bytes, borrowed from runtime State.
 * @param output Caller-owned response storage, left unchanged on failure.
 * @param written Receives the whole response size or zero on failure.
 * @return True when the inputs are valid and the response fits.
 */
[[nodiscard]] bool encode_locate_result(std::uint64_t advertisementId,
                                        std::span<const std::byte> descriptor,
                                        std::span<std::byte> output,
                                        std::size_t& written) noexcept;

} // namespace sunrise::middleware::bap::matchmaking::response
