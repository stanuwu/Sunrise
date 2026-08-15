#include <cstring>

#include "codec.h"
#include "protobuf_wire.h"

namespace sunrise::middleware::protobuf {

Writer::Writer(std::span<std::byte> output) noexcept : output_(output) {}

/** Appends one unsigned varint field after proving it fits. */
bool Writer::write_varint(std::uint32_t fieldNumber, std::uint64_t value) noexcept {
    std::uint64_t key = 0;
    if (!encode_key(fieldNumber, WireType::varint, key)) {
        return false;
    }
    const std::size_t required = varint_size(key) + varint_size(value);
    if (required > output_.size() - size_) {
        return false;
    }

    // Space is proven before the first byte so failure cannot change the prefix.
    std::size_t cursor = size_;
    cursor += write_raw_varint(key, output_.subspan(cursor));
    cursor += write_raw_varint(value, output_.subspan(cursor));
    size_ = cursor;
    return true;
}

/** Appends bytes or an encoded nested message as one length-delimited field. */
bool Writer::write_length_delimited(std::uint32_t fieldNumber,
                                    std::span<const std::byte> value) noexcept {
    std::uint64_t key = 0;
    if (!encode_key(fieldNumber, WireType::lengthDelimited, key)) {
        return false;
    }
    const std::uint64_t length = static_cast<std::uint64_t>(value.size());
    const std::size_t prefixSize = varint_size(key) + varint_size(length);
    const std::size_t available = output_.size() - size_;
    if (prefixSize > available || value.size() > available - prefixSize) {
        return false;
    }

    const std::size_t payloadOffset = size_ + prefixSize;
    if (!value.empty()) {
        // Copy first so payload borrowed from this output survives insertion of its prefix.
        std::memmove(output_.data() + payloadOffset, value.data(), value.size());
    }
    std::size_t cursor = size_;
    cursor += write_raw_varint(key, output_.subspan(cursor));
    cursor += write_raw_varint(length, output_.subspan(cursor));
    size_ = cursor + value.size();
    return true;
}

/** @return Bytes committed to the bound output. */
std::size_t Writer::size() const noexcept {
    return size_;
}

} // namespace sunrise::middleware::protobuf
