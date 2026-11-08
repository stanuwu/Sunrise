#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../schema/generated/tag_schema.h"

// Package file framing: the header and the misc region. No tag class layout lives here.
namespace sunrise::middleware::content::packages::reader::layout {

/** Every installed package file carries this extension. */
inline constexpr std::wstring_view kPackageFileExtension = L".pkg";
/** Only header version 38 is supported. */
inline constexpr std::uint16_t kSupportedVersion = 38;
/** The public header prefix that carries every table offset. */
inline constexpr std::size_t kHeaderSize = 0x180;
/** A decompressed block is always this size. */
inline constexpr std::size_t kBlockSize = 0x40000;
/** Entry table offsets are stored relative to this fixed adjustment. */
inline constexpr std::uint32_t kEntryTableAdjustment = 96;
/** The block table follows the entry table after this gap. */
inline constexpr std::size_t kBlockTableGap = 32;
/** The header's metadata digest is one SHA-1. */
inline constexpr std::size_t kMetadataDigestSize = 20;

/** Header fields read by their fixed public offsets. */
struct HeaderOffsets {
    /** Format version, which the reader checks before trusting any later field. */
    static constexpr std::size_t kVersion = 0x00;
    /** Package id, the high bits of every tag handle the file owns. */
    static constexpr std::size_t kPackageId = 0x04;
    /** Table layout byte: 0 marks a beta package, 1 the layout this reader supports. */
    static constexpr std::size_t kVariant = 0x1A;
    /** Patch index, which picks the newest file of one package. */
    static constexpr std::size_t kPatchId = 0x20;
    /** Logical tag-addressable entry count; the physical region owns a separate prefix count. */
    static constexpr std::size_t kEntryCount = 0xB4;
    /** Logical block count used to bound entry placement reads. */
    static constexpr std::size_t kBlockCount = 0xD0;
    /** File offset of the misc region, which holds the hash64 references. */
    static constexpr std::size_t kMiscOffset = 0xF0;
    /** Byte size of the misc region. */
    static constexpr std::size_t kMiscSize = 0xF4;
    /** Metadata region offset; the entry table starts kEntryTableAdjustment bytes past it. */
    static constexpr std::size_t kMetadataOffset = 0x110;
};

/** Fields of the misc region, relative to its start. */
struct MiscRegion {
    /** Array field of the hash64 reference rows. */
    static constexpr std::size_t kReferenceArray = 48;
    /** A shorter region cannot hold that field, so it carries no references. */
    static constexpr std::size_t kMinimumSize = kReferenceArray + schema::generated::kTagArrayBytes;
};

} // namespace sunrise::middleware::content::packages::reader::layout
