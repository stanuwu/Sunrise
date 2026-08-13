#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::encoding {

/** Encoded 16-bit wire integer width. */
inline constexpr std::size_t kU16Size = sizeof(std::uint16_t);
/** Encoded 32-bit wire integer width. */
inline constexpr std::size_t kU32Size = sizeof(std::uint32_t);
/** Encoded 64-bit wire integer width. */
inline constexpr std::size_t kU64Size = sizeof(std::uint64_t);
/** Wire integers use 8 data bits per byte. */
inline constexpr std::size_t kBitsPerByte = 8;
/** A 16-bit big-endian high byte occupies the remaining wire byte. */
inline constexpr std::size_t kU16HighByteShift = (kU16Size - 1) * kBitsPerByte;
/** The first 32-bit big-endian byte occupies the remaining 3 wire bytes. */
inline constexpr std::size_t kU32ByteZeroShift = (kU32Size - 1) * kBitsPerByte;
/** Each later big-endian byte moves down by one complete wire byte. */
inline constexpr std::size_t kU32ByteOneShift = kU32ByteZeroShift - kBitsPerByte;
/** The third big-endian byte is one wire byte below the second. */
inline constexpr std::size_t kU32ByteTwoShift = kU32ByteOneShift - kBitsPerByte;

/**
 * Reads one unsigned 16-bit big-endian wire value.
 * @param input Exact 2-byte wire field.
 * @return Host-order unsigned value.
 */
[[nodiscard]] inline std::uint16_t
read_u16_be(std::span<const std::byte, kU16Size> input) noexcept {
    return static_cast<std::uint16_t>(
        (std::to_integer<std::uint16_t>(input[0]) << kU16HighByteShift)
        | std::to_integer<std::uint16_t>(input[1]));
}

/**
 * Reverses the four bytes of a 32-bit value.
 * @param value Host-order value.
 * @return The value with its four bytes reversed.
 */
[[nodiscard]] inline constexpr std::uint32_t 
byteswap_u32(std::uint32_t value) noexcept {
    return ((value & 0x000000FFu) << 24) | ((value & 0x0000FF00u) << 8)
           | ((value & 0x00FF0000u) >> 8) | ((value & 0xFF000000u) >> 24);
}

/**
 * Reads one unsigned 32-bit big-endian wire value.
 * @param input Exact 4-byte wire field.
 * @return Host-order unsigned value.
 */
[[nodiscard]] inline std::uint32_t
read_u32_be(std::span<const std::byte, kU32Size> input) noexcept {
    return (std::to_integer<std::uint32_t>(input[0]) << kU32ByteZeroShift)
           | (std::to_integer<std::uint32_t>(input[1]) << kU32ByteOneShift)
           | (std::to_integer<std::uint32_t>(input[2]) << kU32ByteTwoShift)
           | std::to_integer<std::uint32_t>(input[3]);
}

/**
 * Reads one unsigned 32-bit little-endian wire value.
 * @param input Exact 4-byte wire field.
 * @return Host-order unsigned value.
 */
[[nodiscard]] inline std::uint32_t
read_u32_le(std::span<const std::byte, kU32Size> input) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < input.size(); ++index) {
        value |= std::to_integer<std::uint32_t>(input[index]) << (index * kBitsPerByte);
    }
    return value;
}

/**
 * Reads one unsigned 64-bit big-endian wire value.
 * @param input Exact 8-byte wire field.
 * @return Host-order unsigned value.
 */
[[nodiscard]] inline std::uint64_t
read_u64_be(std::span<const std::byte, kU64Size> input) noexcept {
    std::uint64_t value = 0;
    for (const std::byte byte : input) {
        value = (value << kBitsPerByte) | std::to_integer<std::uint64_t>(byte);
    }
    return value;
}

/**
 * Reads one unsigned 64-bit little-endian wire value.
 * @param input Exact 8-byte wire field.
 * @return Host-order unsigned value.
 */
[[nodiscard]] inline std::uint64_t
read_u64_le(std::span<const std::byte, kU64Size> input) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < input.size(); ++index) {
        value |= std::to_integer<std::uint64_t>(input[index]) << (index * kBitsPerByte);
    }
    return value;
}

/**
 * Writes one unsigned 16-bit big-endian wire value.
 * @param output Exact 2-byte wire field.
 * @param value Host-order value to encode.
 */
inline void write_u16_be(std::span<std::byte, kU16Size> output, std::uint16_t value) noexcept {
    output[0] = static_cast<std::byte>(value >> kU16HighByteShift);
    output[1] = static_cast<std::byte>(value);
}

/**
 * Writes one unsigned 32-bit big-endian wire value.
 * @param output Exact 4-byte wire field.
 * @param value Host-order value to encode.
 */
inline void write_u32_be(std::span<std::byte, kU32Size> output, std::uint32_t value) noexcept {
    output[0] = static_cast<std::byte>(value >> kU32ByteZeroShift);
    output[1] = static_cast<std::byte>(value >> kU32ByteOneShift);
    output[2] = static_cast<std::byte>(value >> kU32ByteTwoShift);
    output[3] = static_cast<std::byte>(value);
}

/**
 * Writes one unsigned 64-bit big-endian wire value.
 * @param output Exact 8-byte wire field.
 * @param value Host-order value to encode.
 */
inline void write_u64_be(std::span<std::byte, kU64Size> output, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < output.size(); ++index) {
        const std::size_t shift = (output.size() - index - 1) * kBitsPerByte;
        output[index] = static_cast<std::byte>(value >> shift);
    }
}

} // namespace sunrise::middleware::encoding
