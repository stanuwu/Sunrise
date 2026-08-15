#pragma once

#include <limits>

#include "codec.h"

namespace sunrise::middleware::protobuf {

/** Native buffer sizes must fit the unsigned protobuf length range without narrowing. */
static_assert((std::numeric_limits<std::size_t>::max)()
              <= (std::numeric_limits<std::uint64_t>::max)());

/** Validated field key shared by the protobuf reader. */
struct DecodedKey final {
    std::uint32_t fieldNumber{};
    WireType wireType{WireType::varint};
};

/**
 * Validates and separates one decoded protobuf field key.
 * @param encodedKey Unsigned key value read from the wire.
 * @param key Receives a supported nonzero field key on success.
 * @return True when the field number and wire type are supported.
 */
[[nodiscard]] bool decode_key(std::uint64_t encodedKey, DecodedKey& key) noexcept;

/**
 * Forms one protobuf field key after validating its field number.
 * @param fieldNumber Candidate protobuf field number.
 * @param wireType Supported wire type to place in the key.
 * @param encodedKey Receives the encoded key on success.
 * @return True when the field number is valid.
 */
[[nodiscard]] bool
encode_key(std::uint32_t fieldNumber, WireType wireType, std::uint64_t& encodedKey) noexcept;

/**
 * Reads one bounded uint64 protobuf varint using a local cursor.
 * @param input Complete protobuf message storage.
 * @param cursor Local cursor advanced within the attempted field.
 * @param value Receives the decoded value after the last byte.
 * @return True when the varint terminates without truncation or overflow.
 */
[[nodiscard]] bool
read_varint(std::span<const std::byte> input, std::size_t& cursor, std::uint64_t& value) noexcept;

/**
 * Decodes a little-endian fixed protobuf value from bytes already proven to fit.
 * @param input Fixed-width field bytes.
 * @return Zero-extended numeric field value.
 */
[[nodiscard]] std::uint64_t read_fixed(std::span<const std::byte> input) noexcept;

/**
 * Measures one uint64 protobuf varint.
 * @param value Unsigned value to measure.
 * @return Required encoded byte count.
 */
[[nodiscard]] std::size_t varint_size(std::uint64_t value) noexcept;

/**
 * Writes one varint into storage already proven big enough.
 * @param value Unsigned value to encode.
 * @param output Storage beginning at the field write cursor.
 * @return Encoded byte count.
 */
std::size_t write_raw_varint(std::uint64_t value, std::span<std::byte> output) noexcept;

} // namespace sunrise::middleware::protobuf
