#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::activity_message::entity_slot_request {

/** Activity message type 20 carries the fixed entity-slot request payload. */
inline constexpr std::uint32_t kMessageType = 20;
/** The entity-slot request payload is one encoded 32-bit value. */
inline constexpr std::size_t kEncodedSize = sizeof(std::uint32_t);

/**
 * Decodes the biased signed value from the entity-slot request prefix.
 * @param input Type-20 payload holding the whole value prefix.
 * @param value Cleared first. Receives the decoded value only on success.
 * @return True when the payload holds the whole fixed value.
 */
[[nodiscard]] bool parse_entity_slot_request(std::span<const std::byte> input,
                                             std::int32_t& value) noexcept;

} // namespace sunrise::middleware::bap::activity_message::entity_slot_request
