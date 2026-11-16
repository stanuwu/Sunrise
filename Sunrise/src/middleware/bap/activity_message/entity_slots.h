#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../encoding/bit_reader.h"
#include "../../encoding/bit_writer.h"

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
/** Each reflected mask element is an unsigned 32-bit scalar. */
inline constexpr std::uint8_t kBitsPerMaskWord = 32;
/** The complete lease mask addresses 8,192 entity slots. */
inline constexpr std::size_t kSlotCount = kEncodedSize * kBitsPerMaskByte;

/** Canonical slot bytes; slot i uses byte i / 8 and low bit i % 8. */
using EntitySlotMask = std::array<std::byte, kEncodedSize>;

/** @return True when the mask holds entity slot `slot`, which must be below kSlotCount. */
[[nodiscard]] constexpr bool has_slot(const EntitySlotMask& mask, std::size_t slot) noexcept {
    return (std::to_integer<unsigned>(mask[slot / kBitsPerMaskByte])
            & (1U << (slot % kBitsPerMaskByte)))
           != 0;
}

/**
 * Decodes the fixed big-endian word prefix into canonical slot bytes.
 * @param input Type-21 payload holding the whole mask prefix.
 * @param mask Cleared first. Receives all mask bytes only on success.
 * @return True when the payload holds the whole fixed mask.
 */
[[nodiscard]] bool decode_entity_slots(std::span<const std::byte> input,
                                       EntitySlotMask& mask) noexcept;

/**
 * Encodes canonical slot bytes as 256 big-endian words.
 * @param mask Complete canonical lease mask, chosen by the caller.
 * @param output Caller-owned payload storage, unchanged on failure.
 * @param written Receives 1,024 on success, or zero on failure.
 * @return True when the whole fixed mask fits.
 */
[[nodiscard]] bool encode_entity_slots(std::span<const std::byte, kEncodedSize> mask,
                                       std::span<std::byte> output,
                                       std::size_t& written) noexcept;

/** Reads the fixed u32 array at any bit offset into canonical slot bytes. */
[[nodiscard]] bool read_mask(encoding::bits::Reader& reader, EntitySlotMask& mask) noexcept;
/** Writes canonical slot bytes as fixed 32-bit scalar fields at any bit offset. */
[[nodiscard]] bool write_mask(encoding::bits::Writer& writer,
                              std::span<const std::byte, kEncodedSize> mask) noexcept;

} // namespace sunrise::middleware::bap::activity_message::entity_slots
