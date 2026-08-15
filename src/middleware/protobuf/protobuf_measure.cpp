#include <limits>

#include "codec.h"
#include "protobuf_wire.h"

namespace sunrise::middleware::protobuf {

/** Measures one complete unsigned-varint field after validating its field number. */
bool measure_varint_field(std::uint32_t fieldNumber,
                          std::uint64_t value,
                          std::size_t& size) noexcept {
    size = 0;
    std::uint64_t key = 0;
    if (!encode_key(fieldNumber, WireType::varint, key)) {
        return false;
    }
    size = varint_size(key) + varint_size(value);
    return true;
}

/** Measures one complete length-delimited field after validating its field number. */
bool measure_length_delimited_field(std::uint32_t fieldNumber,
                                    std::size_t payloadSize,
                                    std::size_t& size) noexcept {
    size = 0;
    std::uint64_t key = 0;
    if (!encode_key(fieldNumber, WireType::lengthDelimited, key)) {
        return false;
    }
    const std::size_t prefixSize =
        varint_size(key) + varint_size(static_cast<std::uint64_t>(payloadSize));
    if (payloadSize > (std::numeric_limits<std::size_t>::max)() - prefixSize) {
        return false;
    }
    size = prefixSize + payloadSize;
    return true;
}

} // namespace sunrise::middleware::protobuf
