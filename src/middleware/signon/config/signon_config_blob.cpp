#include "signon_config_blob.h"

#include <algorithm>
#include <cstdint>

namespace sunrise::middleware::signon::config {
namespace {

/** Blob layout the Client's config applier parses; key 0 ends its entry loop. */
constexpr std::uint32_t kMagic = 0x00123456;
constexpr std::uint32_t kTokenKey = 0x007B620F;
constexpr std::uint32_t kTerminator = 0;
/** Byte offset of the token key, immediately after the magic. */
constexpr std::size_t kTokenKeyOffset = 4;
/** Byte offset of the entry length, immediately after the token key. */
constexpr std::size_t kLengthOffset = 8;
/** Byte offset of the token bytes, immediately after the entry length. */
constexpr std::size_t kTokenOffset = 9;

/**
 * Writes one little-endian fixed32 into blob storage.
 * @param output Blob storage.
 * @param offset Byte offset of the value.
 * @param value Value written low byte first.
 */
void write_fixed32(std::span<std::byte> output, std::size_t offset, std::uint32_t value) noexcept {
    output[offset] = static_cast<std::byte>(value);
    output[offset + 1] = static_cast<std::byte>(value >> 8U);
    output[offset + 2] = static_cast<std::byte>(value >> 16U);
    output[offset + 3] = static_cast<std::byte>(value >> 24U);
}

} // namespace

/** Builds the bootstrap config blob around the native token. */
void build(std::span<const std::byte> token, std::span<std::byte> output) noexcept {
    write_fixed32(output, 0, kMagic);
    write_fixed32(output, kTokenKeyOffset, kTokenKey);
    // The entry length is a protobuf varint; 16 encodes in 1 byte.
    output[kLengthOffset] = static_cast<std::byte>(token.size());
    std::copy(token.begin(), token.end(), output.begin() + kTokenOffset);
    write_fixed32(output, kTokenOffset + token.size(), kTerminator);
}

} // namespace sunrise::middleware::signon::config
