#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "../schema/generated/tag_layout_808000_0.h"
#include "../schema/generated/tag_layout_80809e_0.h"
#include "../schema/generated/tag_layout_80809f_0.h"
#include "container_framing.h"
#include "package_file.h"

namespace sunrise::middleware::content::packages::reader::layout {

/** Typed array metadata immediately before each physical package table: marker word, header. */
inline constexpr std::size_t kTableArrayPrefixSize =
    kClassWordSize + sizeof(schema::generated::STypedArrayMarker);
/** Array marker stored at the start of one table prefix. */
inline constexpr std::uint32_t kTableArrayMarker = schema::generated::STypedArrayMarker::kClass;
/** Entry-record element class stored eight bytes before the entry data. */
inline constexpr std::uint32_t kEntryTableElementClass =
    schema::generated::PackageEntryRecord::kClass;
/** Block-record element class stored eight bytes before the block data. */
inline constexpr std::uint32_t kBlockTableElementClass =
    schema::generated::PackageBlockRecord::kClass;
/** Tag handles start at this base. */
inline constexpr std::uint32_t kTagBase = 0x80800000;
/** The package id sits in the bits above the entry index. */
inline constexpr std::uint32_t kTagEntryBits = 13;
/** Entry indices sit in the low bits of a tag handle. */
inline constexpr std::uint32_t kTagEntryMask = (1U << kTagEntryBits) - 1U;
/** Element class of a typed array of u32 words. */
inline constexpr std::uint32_t kU32ArrayClass = schema::generated::U32ArrayRow::kClass;
/** Element class of a typed array of u16 values. */
inline constexpr std::uint32_t kU16ArrayClass = schema::generated::SU16Element::kClass;

/** @return The tag handle of entry `entry` in package `packageId`. */
[[nodiscard]] constexpr std::uint32_t tag_of(std::uint16_t packageId,
                                             std::uint32_t entry) noexcept {
    return kTagBase + (std::uint32_t{packageId} << kTagEntryBits) + entry;
}

/** @return The package bits of a tag handle; the caller checks the tag is at least kTagBase. */
[[nodiscard]] constexpr std::uint32_t package_of(std::uint32_t tag) noexcept {
    return (tag - kTagBase) >> kTagEntryBits;
}

/** @return The entry index of a tag handle. */
[[nodiscard]] constexpr std::uint32_t entry_of_tag(std::uint32_t tag) noexcept {
    return tag & kTagEntryMask;
}

/** Typed array header inside an entry; its rows follow it. */
struct TypedArrayHeader {
    // Offsets from the header start: marker before it, then {u64 count, u32 class, u32 pad}.
    static constexpr std::size_t kMarkerBack = kClassWordSize;
    static constexpr std::size_t kCount = offsetof(schema::generated::STypedArrayMarker, count);
    static constexpr std::size_t kClass =
        offsetof(schema::generated::STypedArrayMarker, elementClass);
    static constexpr std::size_t kPadding = offsetof(schema::generated::STypedArrayMarker, padding);
    static constexpr std::size_t kSize = sizeof(schema::generated::STypedArrayMarker);
};

/** A TagArray field holds a u64 count, then an i64 offset counted from its own position. */
inline constexpr std::size_t kArrayFieldCount =
    offsetof(schema::generated::TagArray<schema::generated::Unknown>, count);
inline constexpr std::size_t kArrayFieldRelative =
    offsetof(schema::generated::TagArray<schema::generated::Unknown>, relative);

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
    std::array<std::byte, 20> sha1{};
    std::array<std::byte, 16> gcmTag{};
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
inline constexpr std::size_t kEntryRecordSize = sizeof(schema::generated::PackageEntryRecord);
/** The block table is walked by this stride, so the record must pack to exactly this width. */
inline constexpr std::size_t kBlockRecordSize = sizeof(schema::generated::PackageBlockRecord);

static_assert(sizeof(EntryRecord) == kEntryRecordSize);
static_assert(sizeof(BlockRecord) == kBlockRecordSize);
static_assert(std::is_trivially_copyable_v<EntryRecord>);
static_assert(std::is_trivially_copyable_v<BlockRecord>);
// Each hand record field sits where the generated package record puts it.
static_assert(offsetof(EntryRecord, reference)
                  == offsetof(schema::generated::PackageEntryRecord, classReference)
              && offsetof(EntryRecord, typeInfo)
                     == offsetof(schema::generated::PackageEntryRecord, typeInfo)
              && offsetof(EntryRecord, blockInfo)
                     == offsetof(schema::generated::PackageEntryRecord, blockInfo));
static_assert(
    offsetof(BlockRecord, offset) == offsetof(schema::generated::PackageBlockRecord, fileOffset)
    && offsetof(BlockRecord, size) == offsetof(schema::generated::PackageBlockRecord, size)
    && offsetof(BlockRecord, patchId) == offsetof(schema::generated::PackageBlockRecord, patchId)
    && offsetof(BlockRecord, flags) == offsetof(schema::generated::PackageBlockRecord, flags)
    && offsetof(BlockRecord, sha1) == offsetof(schema::generated::PackageBlockRecord, sha1)
    && offsetof(BlockRecord, gcmTag) == offsetof(schema::generated::PackageBlockRecord, gcmTag));

/** @param record Entry-table record. @return Its decoded block placement. */
[[nodiscard]] constexpr EntryPlacement placement(const EntryRecord& record) noexcept {
    return EntryPlacement{
        static_cast<std::uint32_t>(record.blockInfo & 0x3FFF),
        static_cast<std::uint32_t>(((record.blockInfo >> 14) & 0x3FFF) << 4),
        static_cast<std::uint32_t>(record.blockInfo >> 28),
    };
}

} // namespace sunrise::middleware::content::packages::reader::layout
