#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace sunrise::middleware::content::packages::reader::layout {

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
/** Tag handles start at this base. */
inline constexpr std::uint32_t kTagBase = 0x80800000;
/** The package id sits in the bits above the entry index. */
inline constexpr std::uint32_t kTagEntryBits = 13;
/** Entry indices sit in the low bits of a tag handle. */
inline constexpr std::uint32_t kTagEntryMask = (1U << kTagEntryBits) - 1U;

/** Header fields read by their fixed public offsets. */
struct HeaderOffsets {
    /** Format version, which the reader checks before trusting any later field. */
    static constexpr std::size_t kVersion = 0x00;
    /** Package id, the high bits of every tag handle the file owns. */
    static constexpr std::size_t kPackageId = 0x04;
    /** Patch index, which picks the newest file of one package. */
    static constexpr std::size_t kPatchId = 0x20;
    /** Entry-table row count. */
    static constexpr std::size_t kEntryCount = 0xB4;
    /** Block-table row count. */
    static constexpr std::size_t kBlockCount = 0xD0;
    /** File offset of the entry table, whose rows this reader indexes by tag. */
    static constexpr std::size_t kEntryTable = 0x110;
};

/** One 16-byte entry-table record. */
struct EntryRecord {
    std::uint32_t reference{};
    std::uint32_t typeInfo{};
    std::uint64_t blockInfo{};
};

/** One 48-byte block-table record. */
struct BlockRecord {
    std::uint32_t offset{};
    std::uint32_t size{};
    std::uint16_t patchId{};
    std::uint16_t flags{};
    std::array<std::byte, 20> opaque{};
    std::array<std::byte, 16> tag{};
};

/** Block flag bits. */
struct BlockFlags {
    /** The block body is Oodle-compressed. */
    static constexpr std::uint16_t kCompressed = 0x1;
    /** The block body is encrypted and needs a block key. */
    static constexpr std::uint16_t kEncrypted = 0x2;
    /** The block uses the alternate key rather than the primary one. */
    static constexpr std::uint16_t kAlternateKey = 0x4;
};

/** Decoded placement of one entry inside the block stream. */
struct EntryPlacement {
    std::uint32_t startBlock{};
    std::uint32_t startOffset{};
    std::uint32_t size{};
};

/** The entry table is walked by this stride, so the record must pack to exactly this width. */
inline constexpr std::size_t kEntryRecordSize = 16;
/** The block table is walked by this stride, so the record must pack to exactly this width. */
inline constexpr std::size_t kBlockRecordSize = 48;

static_assert(sizeof(EntryRecord) == kEntryRecordSize);
static_assert(sizeof(BlockRecord) == kBlockRecordSize);
static_assert(std::is_trivially_copyable_v<EntryRecord>);
static_assert(std::is_trivially_copyable_v<BlockRecord>);

/** @param record Entry-table record. @return Its decoded block placement. */
[[nodiscard]] constexpr EntryPlacement placement(const EntryRecord& record) noexcept {
    return EntryPlacement{
        static_cast<std::uint32_t>(record.blockInfo & 0x3FFF),
        static_cast<std::uint32_t>(((record.blockInfo >> 14) & 0x3FFF) << 4),
        static_cast<std::uint32_t>(record.blockInfo >> 28),
    };
}

} // namespace sunrise::middleware::content::packages::reader::layout
