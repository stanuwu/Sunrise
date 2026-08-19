#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "layout.h"

namespace sunrise::middleware::content::packages::reader {

/** Block key material borrowed for one read. It is never kept, cached or logged. */
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

/** Oodle writes past the requested size, so every destination carries slack. */
inline constexpr std::size_t kBlockSlack = 64;

/**
 * Decoded blocks kept between reads.
 * A tag read decodes a whole block for an entry of a few hundred bytes, and a walk revisits the
 * same block many times, so keeping the last few makes a walk affordable.
 */
inline constexpr std::size_t kBlockCacheSlots = 8;
/** Package headers kept between reads, so one tag read costs no header read of its own. */
inline constexpr std::size_t kHeaderCacheSlots = 8;

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
inline constexpr std::size_t kFileSlots = 4;
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
    std::array<BlockSlot, kBlockCacheSlots> blocks{};
    std::array<HeaderSlot, kHeaderCacheSlots> headers{};
    std::array<FileSlot, kFileSlots> files{};
    std::array<TableSlot, kTableSlots> tables{};
    /** Rising use counter that sets the block cache's replacement order. */
    std::uint64_t useCounter{};
    /** Rising use counter that sets the file and table replacement order. */
    std::uint64_t slotCounter{};
};

/** Everything one tag read needs from the caller. */
struct Source {
    std::wstring_view directory;
    const BlockKeys* keys{};
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
};

/** Visitor for when the package family is part of the extracted domain key. */
using ClassEntryVisitor = bool (*)(void* context, const ClassEntry& entry) noexcept;

/**
 * Reports every installed entry of one tag class.
 * Only the highest patch of each package is scanned, so a tag is reported once.
 * Entry tables are plain file data, so this needs no block keys and runs before any are known.
 * @param directory Installed packages directory.
 * @param classId Tag class to report.
 * @param visitor Required non-owning tag consumer.
 * @param context Caller context passed to the visitor.
 * @param result Receives package, entry and match totals.
 * @return True when the walk and every package header and entry table work.
 */
[[nodiscard]] bool scan_class(std::wstring_view directory,
                              std::uint32_t classId,
                              ClassVisitor visitor,
                              void* context,
                              ScanResult& result) noexcept;

/**
 * Reports every installed entry of one tag class with its package family.
 * Only the highest patch of each package is scanned, so a tag is reported once.
 * @param directory Installed packages directory.
 * @param classId Tag class to report.
 * @param visitor Required non-owning entry consumer.
 * @param context Caller context passed to the visitor.
 * @param result Receives package, entry, and match totals.
 * @return True when the walk and every visitor call work.
 */
[[nodiscard]] bool scan_class_entries(std::wstring_view directory,
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

/**
 * Decodes one complete package block from the highest installed patch.
 * Package patch authoring needs the untouched bytes around one modified entry, so reading only
 * the entry is insufficient.
 * @param source Package directory and borrowed block keys.
 * @param scratch Reader-owned cache and decode storage.
 * @param packageId Package whose latest table selects the block.
 * @param blockIndex Block-table ordinal.
 * @param output Receives the exact decoded block bytes.
 * @param record Receives the latest table row that selected the stored block.
 * @return True when the block authenticates and decompresses completely.
 */
[[nodiscard]] bool read_block(const Source& source,
                              Scratch& scratch,
                              std::uint16_t packageId,
                              std::uint32_t blockIndex,
                              std::vector<std::byte>& output,
                              layout::BlockRecord& record) noexcept;

/**
 * Resolves one stable 64-bit handle through a package's public wide-hash table.
 * @param directory Installed package directory.
 * @param packageId Package whose content contains the wide reference.
 * @param hash64 Stable authored handle.
 * @param tag Receives the installed build's 32-bit tag handle.
 * @param classId Receives the referenced tag class recorded beside the mapping.
 * @return True when the latest package has one structurally valid matching row.
 */
[[nodiscard]] bool resolve_hash64(std::wstring_view directory,
                                  std::uint16_t packageId,
                                  std::uint64_t hash64,
                                  std::uint32_t& tag,
                                  std::uint32_t& classId) noexcept;

} // namespace sunrise::middleware::content::packages::reader
