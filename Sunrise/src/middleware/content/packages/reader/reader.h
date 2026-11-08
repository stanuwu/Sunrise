#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "layout.h"

namespace sunrise::middleware::content::packages::reader {

/** Borrowed block keys must be erased after a read or an explicitly requested export. */
struct BlockKeys {
    std::array<std::byte, 16> primary{};
    std::array<std::byte, 16> alternate{};
    std::array<std::byte, 12> nonceBase{};
};

/** Package paths stay inside fixed storage. */
inline constexpr std::size_t kPathCapacity = 320;

/** One package file path. */
struct Path {
    std::array<wchar_t, kPathCapacity> chars{};
};

/** One package path resolution owned by a single reader. */
struct PackageLocation {
    Path stem{};
    Path latestPath{};
    std::uint32_t patchIndex{};
    bool found{};
};

/** Ends a bucket chain and names no slot. */
inline constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);

/**
 * Key to slot index for one rotating cache.
 * A slot holds one key at a time, so its chain link lives in its own row and a lookup never scans.
 */
struct SlotIndex {
    /** First slot of each bucket, or kNoSlot. The row count is always a power of two. */
    std::vector<std::size_t> buckets{};
    /** Next slot sharing the same bucket, one row per cache slot. */
    std::vector<std::size_t> links{};
};

/** One resolved package held by a reader, keyed by the package id the tag handle names. */
struct PackageLocationSlot {
    PackageLocation location{};
    std::uint16_t packageId{};
    bool held{};
};

/**
 * Package locations one reader holds. 528 packages are installed.
 * Lookup probes forward from the id, so the table must stay well above the installed count.
 */
inline constexpr std::size_t kPackageLocationSlots = 2048;

/** Oodle writes past the requested size, so every destination carries slack. */
inline constexpr std::size_t kBlockSlack = 64;

/**
 * Decoded blocks kept between reads.
 * A tag read decodes a whole block for an entry of a few hundred bytes. A walk revisits the same
 * block many times, so keeping the last few makes a walk affordable.
 */
inline constexpr std::size_t kBlockCacheSlots = 8;
/** Package headers kept between reads, so one tag read costs no header read of its own. */
inline constexpr std::size_t kHeaderCacheSlots = 64;

/** One decoded block and the package block it came from. */
struct BlockSlot {
    std::array<std::byte, layout::kBlockSize + kBlockSlack> bytes{};
    std::size_t size{};
    std::uint64_t key{};
    /** Use order, so the least recently used slot is the one replaced. */
    std::uint64_t used{};
    bool valid{};
};

/** One package header, reduced to the fields an entry read needs. */
struct HeaderSlot {
    std::uint64_t key{};
    std::uint64_t entryTable{};
    std::uint64_t blockTable{};
    std::uint32_t entryCount{};
    std::uint32_t blockCount{};
    bool valid{};
};

/** Package files one reader keeps open. A read reaches the package and any patch that holds it. */
inline constexpr std::size_t kFileSlots = 64;
/** Packages whose tables one reader holds. A pass reads one package before moving to the next. */
inline constexpr std::size_t kTableSlots = 2;
/** A tag handle indexes 13 bits of entries, so no package declares more than this. */
inline constexpr std::size_t kEntryCapacity = 1U << layout::kTagEntryBits;
/** The widest installed block table is 4,312 rows. */
inline constexpr std::size_t kBlockCapacity = 8192;

/** One package file one reader keeps open. */
struct FileSlot {
    Path path{};
    /** Open file, or null for a free slot. The Windows handle type stays out of this header. */
    void* file{};
    std::uint64_t hash{};
    /** Use order, so the least recently used slot is the one replaced. */
    std::uint64_t used{};
};

/** One package's entry and block tables, held so a tag read costs no file read of its own. */
struct TableSlot {
    Path path{};
    /** Package and patch this slot holds, so a lookup compares no path text. */
    std::uint64_t key{};
    std::array<layout::EntryRecord, kEntryCapacity> entries{};
    std::array<layout::BlockRecord, kBlockCapacity> blocks{};
    std::uint32_t entryCount{};
    std::uint32_t blockCount{};
    std::uint64_t used{};
    bool occupied{};
};

/**
 * Everything one reader owns, kept off the caller stack.
 * No two readers share a scratch, so a parallel pass gives each thread its own and nothing in
 * the read path locks. It is several megabytes: hold it as static or member storage.
 */
struct Scratch {
    std::array<std::byte, layout::kBlockSize> ciphertext{};
    std::array<std::byte, layout::kBlockSize> plaintext{};
    std::array<std::byte, layout::kBlockSize + kBlockSlack> decompressed{};
    /**
     * Decoded blocks, sized by prepare_blocks. A pass that walks many scenarios of one destination
     * re-reads the same blocks, so its cache is worth more than the default.
     */
    std::vector<BlockSlot> blocks{};
    /** Slot of each cached block key, so a large cache costs no scan. */
    SlotIndex blockIndex{};
    /** Next slot to replace once the cache is full. */
    std::size_t blockCursor{};
    std::array<HeaderSlot, kHeaderCacheSlots> headers{};
    std::array<FileSlot, kFileSlots> files{};
    /**
     * Package tables, sized by prepare_caches. A miss re-reads the whole entry and block
     * table, so a pass that alternates packages pays half a megabyte of file reads per switch.
     */
    std::vector<TableSlot> tables{};
    /** Slot of each held package, keyed by package and patch so no path is compared. */
    SlotIndex tableIndex{};
    /** Next table slot to replace once every slot is held. */
    std::size_t tableCursor{};
    /** Package locations resolved for this reader's current source directory. */
    std::vector<PackageLocationSlot> packageLocations{};
    /** Owned copy of the source directory; changing Source clears the locations above. */
    Path packageDirectory{};
    /** Characters held in packageDirectory. Zero when the directory did not fit. */
    std::size_t packageDirectoryLength{};
    /** Source mode the locations, headers, tables and blocks were read under. */
    bool packageInstalledOnly{};
    /** Overlay epoch the locations were resolved under. */
    std::uint64_t overlayEpoch{};
    /** Preserves a successful read if caching its location runs out of memory. */
    PackageLocation packageLocationFallback{};
    /** Rising use counter that sets the block cache's replacement order. */
    std::uint64_t useCounter{};
    /** Rising use counter that sets the file and table replacement order. */
    std::uint64_t slotCounter{};
    /** Block cache hits and misses, so the read path is measured rather than assumed. */
    std::uint64_t blockHits{};
    std::uint64_t blockMisses{};
    /** Bytes of block this scratch decoded, cache hits excluded. */
    std::uint64_t blockBytes{};
    /** Per-reader package-location hits and cold resolutions. */
    std::uint64_t packageLocationHits{};
    std::uint64_t packageLocationMisses{};
};

/**
 * Sizes one reader's block cache and drops whatever it held.
 * @param scratch Reader whose cache is resized.
 * @param slots Blocks to keep. Each costs 256 KiB, and zero restores the small default.
 * @return True when the storage was reserved.
 */
[[nodiscard]] bool prepare_blocks(Scratch& scratch, std::size_t slots) noexcept;

/**
 * Sizes one reader's package-table cache and drops whatever it held.
 * @param scratch Reader whose cache is resized.
 * @param slots Packages to hold. Each costs 512 KiB, and zero restores the small default.
 * @return True when the storage was reserved.
 */
[[nodiscard]] bool prepare_tables(Scratch& scratch, std::size_t slots) noexcept;

/** Everything one tag read needs from the caller. */
struct Source {
    std::wstring_view directory;
    const BlockKeys* keys{};
    /** Reads installed package files only and ignores every file a mod serves. */
    bool installedOnly{};
};

/** Totals from one whole class scan. */
struct ScanResult {
    std::size_t packages{};
    std::size_t entries{};
    std::size_t matches{};
};

/** Visitor called once per matching tag. Returning false stops the scan and fails it. */
using ClassVisitor = bool (*)(void* context, std::uint32_t tag) noexcept;

/** One class-scan match and the package family that owns it. */
struct ClassEntry {
    std::uint32_t tag{};
    /** Leaf name without the patch suffix or extension. Valid only during the visitor call. */
    std::wstring_view packageFamily;
    /** Highest installed patch selected for this package family. */
    std::uint32_t patchIndex{};
};

/** Visitor for when the package family is part of the extracted domain key. */
using ClassEntryVisitor = bool (*)(void* context, const ClassEntry& entry) noexcept;

/**
 * Reports every installed entry of one tag class.
 * Only the highest patch of each package is scanned, so a tag is reported once.
 * Entry tables are plain file data, so this needs no block keys and runs before any are known.
 * @param source Installed packages directory and read mode; block keys are not read.
 * @param classId Tag class to report.
 * @param visitor Required non-owning tag consumer.
 * @param context Caller context passed to the visitor.
 * @param result Receives package, entry and match totals.
 * @return True when the walk and every package header and entry table work.
 */
[[nodiscard]] bool scan_class(const Source& source,
                              std::uint32_t classId,
                              ClassVisitor visitor,
                              void* context,
                              ScanResult& result) noexcept;

/**
 * Reports every installed entry of one tag class with its package family.
 * Only the highest patch of each package is scanned, so a tag is reported once.
 * @param source Installed packages directory and read mode; block keys are not read.
 * @param classId Tag class to report.
 * @param visitor Required non-owning entry consumer.
 * @param context Caller context passed to the visitor.
 * @param result Receives package, entry, and match totals.
 * @return True when the walk and every visitor call work.
 */
[[nodiscard]] bool scan_class_entries(const Source& source,
                                      std::uint32_t classId,
                                      ClassEntryVisitor visitor,
                                      void* context,
                                      ScanResult& result) noexcept;

/**
 * Closes the package files the class sweeps keep open.
 * A finished pass says when that storage is no longer wanted. A later read opens and reads again.
 */
void release_caches() noexcept;

/**
 * Closes the package files one reader keeps open and drops its held tables.
 * @param scratch The reader's own storage.
 */
void close_files(Scratch& scratch) noexcept;

/**
 * Reports one tag's package entry class without reading, decrypting, or decompressing its
 * body.
 * @param source Installed package directory; block keys are not consumed.
 * @param scratch Reader-owned package location and table caches.
 * @param tag Tag whose entry-table row is inspected.
 * @param classId Receives the physical class recorded by the package entry.
 * @return True when the tag and its entry-table row resolve.
 */
[[nodiscard]] bool read_tag_class(const Source& source,
                                  Scratch& scratch,
                                  std::uint32_t tag,
                                  std::uint32_t& classId) noexcept;

/**
 * Reads one tagged entry without reporting its class.
 * The entry declares its own size, so the destination grows to fit it.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param tag Tag handle naming the package and entry.
 * @param output Receives exactly the entry bytes.
 * @return True when every block authenticates and decompresses.
 */
[[nodiscard]] bool read_tag(const Source& source,
                            Scratch& scratch,
                            std::uint32_t tag,
                            std::vector<std::byte>& output) noexcept;

/**
 * Reads one tagged entry and reports the class the entry table records for it.
 * A chain that hops between blob classes needs the class to pick the parser, and the entry
 * record already carries it, so no second read is needed.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param tag Tag handle naming the package and entry.
 * @param output Receives exactly the entry bytes.
 * @param classId Receives the class the entry table records for this tag.
 * @return True when every block authenticates and decompresses.
 */
[[nodiscard]] bool read_tag(const Source& source,
                            Scratch& scratch,
                            std::uint32_t tag,
                            std::vector<std::byte>& output,
                            std::uint32_t& classId) noexcept;

} // namespace sunrise::middleware::content::packages::reader
