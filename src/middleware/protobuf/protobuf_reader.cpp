#include <limits>

#include "codec.h"
#include "protobuf_wire.h"

namespace sunrise::middleware::protobuf {
namespace {

/** Fixed32 occupies 4 little-endian wire bytes. */
constexpr std::size_t kFixed32Size = 4;
/** Fixed64 occupies 8 little-endian wire bytes. */
constexpr std::size_t kFixed64Size = 8;

} // namespace

Reader::Reader(std::span<const std::byte> input) noexcept : input_(input) {}

/** Decodes one supported field without changing state on malformed input. */
bool Reader::next(Field& field) noexcept {
    std::size_t cursor = cursor_;
    std::uint64_t encodedKey = 0;
    DecodedKey key{};
    if (!read_varint(input_, cursor, encodedKey) || !decode_key(encodedKey, key)) {
        return false;
    }

    Field decoded{};
    decoded.fieldNumber = key.fieldNumber;
    std::uint64_t encodedLength = 0;
    std::size_t length = 0;
    switch (key.wireType) {
    case WireType::varint:
        decoded.wireType = WireType::varint;
        if (!read_varint(input_, cursor, decoded.value)) {
            return false;
        }
        break;
    case WireType::fixed64:
        if (kFixed64Size > input_.size() - cursor) {
            return false;
        }
        decoded.wireType = WireType::fixed64;
        decoded.value = read_fixed(input_.subspan(cursor, kFixed64Size));
        cursor += kFixed64Size;
        break;
    case WireType::lengthDelimited:
        if (!read_varint(input_, cursor, encodedLength)
            || encodedLength > (std::numeric_limits<std::size_t>::max)()) {
            return false;
        }
        length = static_cast<std::size_t>(encodedLength);
        if (length > input_.size() - cursor) {
            return false;
        }
        decoded.wireType = WireType::lengthDelimited;
        decoded.bytes = input_.subspan(cursor, length);
        cursor += length;
        break;
    case WireType::fixed32:
        if (kFixed32Size > input_.size() - cursor) {
            return false;
        }
        decoded.wireType = WireType::fixed32;
        decoded.value = read_fixed(input_.subspan(cursor, kFixed32Size));
        cursor += kFixed32Size;
        break;
    default:
        return false;
    }

    // The public cursor and output field change together only after full validation.
    cursor_ = cursor;
    field = decoded;
    return true;
}

/** @return Bytes after the last successfully decoded field. */
std::size_t Reader::remaining() const noexcept {
    return input_.size() - cursor_;
}

} // namespace sunrise::middleware::protobuf
