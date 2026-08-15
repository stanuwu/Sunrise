#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::activity_message::entity_slots {

/** Activity message type 0 carries this payload in a server notification. */
inline constexpr std::uint32_t kNotificationMessageType = 0;
/** Activity message type 21 carries this payload in a client request. */
inline constexpr std::uint32_t kRequestMessageType = 21;
/** The fixed entity-slot payload contains 256 encoded 32-bit words. */
inline constexpr std::size_t kWordCount = 256;
/** The fixed entity-slot payload is exactly 1,024 bytes. */
inline constexpr std::size_t kEncodedSize = kWordCount * sizeof(std::uint32_t);
/** Each mask byte carries 8 entity-slot lease flags. */
inline constexpr std::size_t kBitsPerMaskByte = 8;
/** The complete lease mask addresses 8,192 entity slots. */
inline constexpr std::size_t kSlotCount = kEncodedSize * kBitsPerMaskByte;

/** Wire-order bytes; slot i uses byte i / 8 and low bit i % 8. */
using EntitySlotMask = std::array<std::byte, kEncodedSize>;

/**
 * Decodes the fixed entity-slot prefix without changing its wire byte order.
 * @param input Type-21 payload holding the whole mask prefix.
 * @param mask Cleared first. Receives all mask bytes only on success.
 * @return True when the payload holds the whole fixed mask.
 */
[[nodiscard]] bool decode_entity_slots(std::span<const std::byte> input,
                                       EntitySlotMask& mask) noexcept;

/**
 * Encodes one selected entity-slot lease mask without changing its wire byte order.
 * @param mask Complete wire-order lease mask, chosen by the caller.
 * @param output Caller-owned payload storage, unchanged on failure.
 * @param written Receives 1,024 on success, or zero on failure.
 * @return True when the whole fixed mask fits.
 */
[[nodiscard]] bool encode_entity_slots(std::span<const std::byte, kEncodedSize> mask,
                                       std::span<std::byte> output,
                                       std::size_t& written) noexcept;

} // namespace sunrise::middleware::bap::activity_message::entity_slots
