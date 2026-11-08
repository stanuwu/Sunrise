#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace sunrise::middleware::content::packages::reader {

/** Texture header entry, package type 32; it has no tag class. */
struct TextureHeader final {
    /** Bytes of the pixel data across both buffers. */
    std::uint32_t dataSize;
    /** DXGI format of the pixels. */
    std::uint32_t format;
    std::array<std::byte, 4> unknown8;
    /** Always kTextureHeaderMarker. */
    std::uint16_t marker;
    std::uint16_t width;
    std::uint16_t height;
    std::uint16_t depth;
    std::uint16_t arraySize;
    std::array<std::byte, 1> unknown16;
    std::uint8_t mipCount;
    /** Leading mips stored in the large buffer. */
    std::uint8_t largeMipCount;
    std::array<std::byte, 11> unknown19;
    /** Tag of the streamed large-mip buffer, or kNoLargeBuffer. */
    std::uint32_t largeBuffer;
};

/** Marker word every texture header carries. */
inline constexpr std::uint16_t kTextureHeaderMarker = 0xCAFE;
/** Large-buffer tag of a texture that streams no mips. */
inline constexpr std::uint32_t kNoLargeBuffer = 0xFFFFFFFFU;
/** DXGI_FORMAT_R8G8B8A8_UNORM. */
inline constexpr std::uint32_t kTextureFormatRgba8 = 28;

/** A texture header entry is 40 bytes. */
inline constexpr std::size_t kTextureHeaderBytes = 40;

// No padding and the full size pin every field to its package offset.
static_assert(std::has_unique_object_representations_v<TextureHeader>);
static_assert(sizeof(TextureHeader) == kTextureHeaderBytes);

} // namespace sunrise::middleware::content::packages::reader
