#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::protobuf {

/** Supported primitive protobuf wire types; group encodings are rejected. */
enum class WireType : std::uint8_t {
    /** Base-128 unsigned integer wire code. */
    varint = 0,
    /** Little-endian 64-bit wire code. */
    fixed64 = 1,
    /** Length-prefixed byte or nested-message wire code. */
    lengthDelimited = 2,
    /** Little-endian 32-bit wire code. */
    fixed32 = 5,
};

/**
 * One decoded protobuf field borrowing any length-delimited payload.
 * Numeric wire types use value; length-delimited fields use bytes.
 */
struct Field final {
    std::uint32_t fieldNumber{};
    WireType wireType{WireType::varint};
    std::uint64_t value{};
    std::span<const std::byte> bytes{};
};

/**
 * Measures one complete unsigned-varint field after validating its field number.
 * @param fieldNumber Nonzero protobuf field number.
 * @param value Unsigned field value.
 * @param size Cleared output receiving the complete encoded field size.
 * @return True when the field key and resulting size are representable.
 */
[[nodiscard]] bool
measure_varint_field(std::uint32_t fieldNumber, std::uint64_t value, std::size_t& size) noexcept;

/**
 * Measures one complete length-delimited field after validating its field number.
 * @param fieldNumber Nonzero protobuf field number.
 * @param payloadSize Encoded byte count of the field payload.
 * @param size Cleared output receiving key, length, and payload bytes.
 * @return True when the field key, payload length, and resulting size are representable.
 */
[[nodiscard]] bool measure_length_delimited_field(std::uint32_t fieldNumber,
                                                  std::size_t payloadSize,
                                                  std::size_t& size) noexcept;

/** Allocation-free iterator over one complete protobuf message. */
class Reader final {
public:
    /** @param input Complete immutable protobuf message storage. */
    explicit Reader(std::span<const std::byte> input) noexcept;

    /**
     * Decodes the next complete field and commits only after validation.
     * @param field Receives the field on success and remains unchanged on failure.
     * @return True for one field; false at end or on malformed remaining input.
     */
    [[nodiscard]] bool next(Field& field) noexcept;

    /** @return Bytes after the last successfully decoded field. */
    [[nodiscard]] std::size_t remaining() const noexcept;

private:
    std::span<const std::byte> input_;
    std::size_t cursor_{};
};

/** Allocation-free protobuf encoder over fixed caller-owned storage. */
class Writer final {
public:
    /** @param output Fixed caller-owned protobuf storage. */
    explicit Writer(std::span<std::byte> output) noexcept;

    /**
     * Appends one unsigned varint field after proving it fits.
     * @param fieldNumber Nonzero protobuf field number.
     * @param value Unsigned field value.
     * @return True when the complete field fits and is committed.
     */
    [[nodiscard]] bool write_varint(std::uint32_t fieldNumber, std::uint64_t value) noexcept;

    /**
     * Appends bytes or an encoded nested message as one length-delimited field.
     * @param fieldNumber Nonzero protobuf field number.
     * @param value Borrowed payload copied into the output.
     * @return True when the complete field fits and is committed.
     */
    [[nodiscard]] bool write_length_delimited(std::uint32_t fieldNumber,
                                              std::span<const std::byte> value) noexcept;

    /** @return Bytes committed to the bound output. */
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::span<std::byte> output_;
    std::size_t size_{};
};

} // namespace sunrise::middleware::protobuf
