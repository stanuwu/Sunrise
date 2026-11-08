#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "../reader/layout.h"
#include "merge.h"

namespace sunrise::middleware::content::packages::merge::internal {

/** Header fields the merge reads or rewrites, by their fixed offsets. */
struct HeaderField final {
    // Offsets in the 0x180-byte version-38 header.
    static constexpr std::size_t kVersion = reader::layout::HeaderOffsets::kVersion;
    static constexpr std::size_t kPlatform = 0x02;
    static constexpr std::size_t kPackageId = reader::layout::HeaderOffsets::kPackageId;
    static constexpr std::size_t kBuildSignature = 0x08;
    /** Byte 1 marks the table layout this merge reads and writes. */
    static constexpr std::size_t kVariant = reader::layout::HeaderOffsets::kVariant;
    static constexpr std::size_t kPatchId = reader::layout::HeaderOffsets::kPatchId;
    static constexpr std::size_t kSignatureOffset = 0xB0;
    static constexpr std::size_t kEntryCount = reader::layout::HeaderOffsets::kEntryCount;
    static constexpr std::size_t kBlockCount = reader::layout::HeaderOffsets::kBlockCount;
    static constexpr std::size_t kMiscOffset = reader::layout::HeaderOffsets::kMiscOffset;
    static constexpr std::size_t kMiscSize = reader::layout::HeaderOffsets::kMiscSize;
    static constexpr std::size_t kMiscDigest = 0xF8;
    static constexpr std::size_t kMetadataOffset = reader::layout::HeaderOffsets::kMetadataOffset;
    static constexpr std::size_t kMetadataSize = 0x114;
    static constexpr std::size_t kMetadataDigest = 0x118;
    static constexpr std::size_t kFooterOffset = 0x160;
    static constexpr std::size_t kFileSize = 0x164;
};

/** Metadata fields, relative to the start of the metadata region. */
struct MetadataField final {
    // The region starts with its own byte size.
    static constexpr std::size_t kTotalSize = 0x00;
    /** Always 1 in the variant-1 layout. */
    static constexpr std::size_t kLayout = 0x08;
    /** Count and relative-pointer pairs for the entry and block tables. */
    static constexpr std::size_t kEntryCount = 0x10;
    static constexpr std::size_t kEntryPointer = 0x18;
    static constexpr std::size_t kBlockCount = 0x20;
    static constexpr std::size_t kBlockPointer = 0x28;
    /** A nonzero count means a relative pointer follows, past the block table. */
    static constexpr std::size_t kTagPairCount = 0x30;
    static constexpr std::size_t kTagPairPointer = 0x38;
    static constexpr std::size_t kEntryTable = reader::layout::kEntryTableAdjustment;
};

/** Fields inside one block record: file offset, stored size and patch id. */
struct BlockRecordField final {
    // Package format offsets, from the generated record layout.
    static constexpr std::size_t kOffset =
        offsetof(schema::generated::PackageBlockRecord, fileOffset);
    static constexpr std::size_t kSize = offsetof(schema::generated::PackageBlockRecord, size);
    static constexpr std::size_t kPatchId =
        offsetof(schema::generated::PackageBlockRecord, patchId);
    static constexpr std::size_t kDigest = offsetof(schema::generated::PackageBlockRecord, sha1);
};

/** The placement word inside one entry record. */
struct EntryRecordField final {
    // Package format offset, from the generated record layout.
    static constexpr std::size_t kPlacement =
        offsetof(schema::generated::PackageEntryRecord, blockInfo);
};

/** Offsets of the typed array prefix fields, back from the first table record. */
struct ArrayPrefixField final {
    // A marker word, then the array header: count, element class and padding.
    static constexpr std::size_t kMarker = reader::layout::kTableArrayPrefixSize;
    static constexpr std::size_t kCapacity =
        reader::layout::TypedArrayHeader::kSize - reader::layout::TypedArrayHeader::kCount;
    static constexpr std::size_t kElementClass =
        reader::layout::TypedArrayHeader::kSize - reader::layout::TypedArrayHeader::kClass;
};

/** Header values that name the layout this merge supports. */
inline constexpr std::uint16_t kSupportedPlatform = 2;
inline constexpr std::uint8_t kSupportedVariant = 1;
inline constexpr std::uint64_t kSupportedMetadataLayout = 1;
/** A block handle indexes 14 bits of blocks. */
inline constexpr std::size_t kBlockLimit = std::size_t{1} << 14;
/** The patch field takes one byte in the client's header check. */
inline constexpr std::uint32_t kPatchLimit = 0xFF;
/** Bound on one misc or metadata region. The largest installed table region is near 1 MiB. */
inline constexpr std::size_t kRegionLimit = std::size_t{16} << 20;
/** Regions and blocks start on this disk alignment. */
inline constexpr std::uint64_t kDiskAlignment = 0x800;
/** The footer fills one alignment unit and starts with this marker. */
inline constexpr std::uint32_t kFooterMarker = 0xDEADBEEFU;
/** Entry placements keep the start block in their low 14 bits. */
inline constexpr std::uint64_t kStartBlockMask = kBlockLimit - 1;

/** @return `value` rounded up to `unit`, a power of two. Package limits prevent overflow. */
[[nodiscard]] constexpr std::uint64_t align(std::uint64_t value,
                                            std::uint64_t unit = kDiskAlignment) noexcept {
    return (value + unit - 1) & ~(unit - 1);
}

/**
 * Packs one entry placement: size from bit 28, offset in 16-byte units from bit 14, start block.
 * @return The placement word that reader::layout::placement decodes.
 */
[[nodiscard]] constexpr std::uint64_t
encode_placement(std::uint64_t startBlock, std::uint64_t startOffset, std::uint64_t size) noexcept {
    return (size << 28) | ((startOffset / 16) << 14) | startBlock;
}

/** One package file's header and table regions. */
struct Package final {
    std::array<std::byte, reader::layout::kHeaderSize> header{};
    std::vector<std::byte> misc;
    std::vector<std::byte> metadata;
    std::uint16_t packageId{};
    std::uint16_t patchId{};
    std::uint32_t entryCount{};
    std::uint32_t blockCount{};
    /** Metadata offset of block record 0. */
    std::size_t blockTable{};
    std::size_t blockCapacity{};
};

/** @return The little-endian value at `offset`. The caller has checked the bounds. */
template <typename Value>
[[nodiscard]] Value load(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    Value value{};
    std::memcpy(&value, bytes.data() + offset, sizeof value);
    return value;
}

/** Writes one little-endian value at `offset`. The caller has checked the bounds. */
template <typename Value>
void store(std::span<std::byte> bytes, std::size_t offset, Value value) noexcept {
    std::memcpy(bytes.data() + offset, &value, sizeof value);
}

/** Reads packed little-endian records front to back and refuses any read past the end. */
class Cursor final {
public:
    explicit Cursor(std::span<const std::byte> bytes, std::size_t at = 0) noexcept
        : bytes_(bytes), at_(at) {}

    template <typename Value> [[nodiscard]] bool take(Value& output) noexcept {
        if (!fits(1, sizeof(Value))) {
            return false;
        }
        std::memcpy(&output, bytes_.data() + at_, sizeof(Value));
        at_ += sizeof(Value);
        return true;
    }

    [[nodiscard]] bool take(std::size_t count, std::span<const std::byte>& output) noexcept {
        if (!fits(count, 1)) {
            return false;
        }
        output = bytes_.subspan(at_, count);
        at_ += count;
        return true;
    }

    /** Reads `count` values. Throws std::bad_alloc. */
    template <typename Value>
    [[nodiscard]] bool take(std::size_t count, std::vector<Value>& output) {
        if (!fits(count, sizeof(Value))) {
            return false;
        }
        output.resize(count);
        for (Value& value : output) {
            (void)take(value);
        }
        return true;
    }

    /** @return True when `count` records of `size` bytes can still follow. */
    [[nodiscard]] bool fits(std::uint64_t count, std::size_t size) const noexcept {
        return at_ <= bytes_.size() && count <= (bytes_.size() - at_) / size;
    }

    [[nodiscard]] bool done() const noexcept {
        return at_ == bytes_.size();
    }

    void skip_to(std::size_t at) noexcept {
        at_ = at;
    }

    [[nodiscard]] std::size_t at() const noexcept {
        return at_;
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t at_{};
};

/**
 * Reads and checks one variant-1 package's header, misc and metadata regions.
 * @param path Package file.
 * @param output Receives the regions and the table positions.
 * @return ok, unreadable, or format.
 */
[[nodiscard]] Status read_package(const wchar_t* path, Package& output) noexcept;

/** @param file Borrowed open handle. @param output Receives tables. @return The read status. */
[[nodiscard]] Status read_package(void* file, Package& output) noexcept;

/**
 * Checks one patch against its base and marks the entries it changes.
 * @return ok, or the first rule the patch breaks.
 */
[[nodiscard]] Status
check(const Package& base, const Package& patch, std::bitset<kEntryLimit>& changed) noexcept;

/**
 * Reads an exact byte range from one file.
 * @param file Open Windows handle, typed as void so this header stays free of Windows.h.
 * @return False on a short read.
 */
[[nodiscard]] bool
read_exact(void* file, std::uint64_t offset, std::span<std::byte> output) noexcept;

/**
 * Writes one byte range at an absolute file offset.
 * @param file Open Windows handle.
 * @return False on a short write. An empty range is true.
 */
[[nodiscard]] bool
write_at(void* file, std::uint64_t offset, std::span<const std::byte> bytes) noexcept;

/**
 * Copies the base metadata with its block table grown to hold `blockCount` records.
 * Sets the total size, block count and capacity, and moves the tag-pair pointer.
 * @param output Receives the grown metadata. Throws std::bad_alloc.
 * @return False when the tag pairs do not sit past the old block table.
 */
[[nodiscard]] bool
grow_block_table(const Package& base, std::size_t blockCount, std::vector<std::byte>& output);

/** @return The 16-byte entry record `index`. The caller has checked the index. */
[[nodiscard]] std::span<const std::byte> entry_bytes(const Package& package,
                                                     std::uint32_t index) noexcept;

/** @return The 48-byte block record `index`. The caller has checked the index. */
[[nodiscard]] std::span<const std::byte> block_bytes(const Package& package,
                                                     std::uint32_t index) noexcept;

} // namespace sunrise::middleware::content::packages::merge::internal
