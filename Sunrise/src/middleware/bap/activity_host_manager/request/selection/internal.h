#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../../encoding/bit_reader.h"
#include "definition.h"

namespace sunrise::middleware::bap::activity_host_manager::request::selection {

/**
 * Decodes one field-one payload. Its transaction and wire bytes are not kept.
 * @param input Complete field-one bytes taken from the service-6 protobuf.
 * @param selection Receives safe scalars only after the whole payload checks out.
 * @return True when the opcode, descriptor, dynamic counts and tail are all valid.
 */
[[nodiscard]] bool parse_field_one(std::span<const std::byte> input,
                                   ActivityManagerSelection& selection) noexcept;

/**
 * Decodes the second copy of the descriptor, which rides inside protobuf field two. Everything
 * ahead of it is fixed width except the leading session token, whose own count field gives its
 * length.
 * @param input Complete field-two bytes taken from the service-6 protobuf.
 * @param selection Receives safe scalars only after the descriptor checks out.
 * @return True when the fixed prefix and the descriptor are both complete.
 */
[[nodiscard]] bool parse_field_two(std::span<const std::byte> input,
                                   ActivityManagerSelection& selection) noexcept;

/**
 * Copies one bit range out of a bit-packed buffer, left-aligned into byte storage.
 * @param source Complete bit-packed storage.
 * @param startBit First bit to copy.
 * @param bitCount Bits to copy.
 * @param output Receives the left-aligned copy.
 * @param outputBitLength Receives bitCount, or zero when the range does not fit.
 * @return True when the range is inside the source and fits the output.
 */
[[nodiscard]] bool copy_bits(std::span<const std::byte> source,
                             std::size_t startBit,
                             std::size_t bitCount,
                             std::span<std::byte> output,
                             std::size_t& outputBitLength) noexcept;

/**
 * Reads one optional-field presence bit.
 * @param reader Bounded MSB-first descriptor reader.
 * @param present Receives true when the optional field follows.
 * @return True when the presence bit was available.
 */
[[nodiscard]] bool read_presence(encoding::bits::Reader& reader, bool& present) noexcept;

/**
 * Reads one biased signed-byte schema field with checked narrowing.
 * @param reader Bounded MSB-first descriptor reader.
 * @param width Field width required by the schema.
 * @param bias Positive value added by the wire encoder.
 * @param value Receives the decoded signed-byte value.
 * @return True when the field is complete and representable.
 */
[[nodiscard]] bool read_i8(encoding::bits::Reader& reader,
                           std::uint8_t width,
                           std::int16_t bias,
                           std::int8_t& value) noexcept;

/**
 * Reads one biased signed-short schema field with checked narrowing.
 * @param reader Bounded MSB-first descriptor reader.
 * @param width Field width required by the schema.
 * @param bias Positive value added by the wire encoder.
 * @param value Receives the decoded signed-short value.
 * @return True when the field is complete and representable.
 */
[[nodiscard]] bool read_i16(encoding::bits::Reader& reader,
                            std::uint8_t width,
                            std::int16_t bias,
                            std::int16_t& value) noexcept;

/**
 * Skips one optional opaque field after checking its presence and width.
 * @param reader Bounded MSB-first descriptor reader.
 * @param width Wire width used when the field is present.
 * @return True when the presence marker and the optional payload are complete.
 */
[[nodiscard]] bool skip_optional(encoding::bits::Reader& reader, std::size_t width) noexcept;

/**
 * Reads one optional unsigned 32-bit hash.
 * @param reader Bounded MSB-first descriptor reader.
 * @param present Receives whether the hash field was present.
 * @param value Receives the unsigned hash only when present.
 * @return True when the presence marker and the optional hash are complete.
 */
[[nodiscard]] bool
read_hash(encoding::bits::Reader& reader, bool& present, std::uint32_t& value) noexcept;

/**
 * Reads one complete descriptor into safe scalar-only output.
 * @param reader Bounded MSB-first reader positioned at the descriptor.
 * @param selection Temporary output that receives only the approved scalar fields.
 * @return True when every fixed field, presence marker and array is complete.
 */
[[nodiscard]] bool parse(encoding::bits::Reader& reader,
                         ActivityManagerSelection& selection) noexcept;

/**
 * Reads the descriptor suffix shared with activity-message type 11. The reader must start at
 * the
 * five-bit skull count, after both optional identity fields.
 * @param reader Bounded
 * MSB-first reader positioned at the skull count.
 * @param selection Receives skull, destination,
 * package, and tail scalars.
 * @return True when the complete continuation is present and
 * structurally valid.
 */
[[nodiscard]] bool parse_continuation(encoding::bits::Reader& reader,
                                      ActivityManagerSelection& selection) noexcept;

} // namespace sunrise::middleware::bap::activity_host_manager::request::selection
