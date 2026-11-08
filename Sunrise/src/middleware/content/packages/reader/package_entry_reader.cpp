#include <Windows.h>

#include <algorithm>
#include <array>
#include <vector>

#include "../../../compression/oodle/runtime.h"
#include "../../../crypto/aes_gcm_decrypt.h"
#include "block_cache.h"
#include "handle_cache.h"
#include "internal.h"
#include "package_table_cache.h"

namespace sunrise::middleware::content::packages::reader {
namespace {

/** The second nonce byte is fixed for this package branch. */
constexpr std::byte kNonceBranchByte{0xF9};

/** Zeroes one derived package nonce when its scope ends, including every failure return. */
struct NonceWiper final {
    std::array<std::byte, crypto::aes_gcm::kNonceSize>& nonce;

    ~NonceWiper() noexcept {
        SecureZeroMemory(nonce.data(), nonce.size());
    }
};

/**
 * Builds the package nonce from the borrowed base and the package id.
 * @param base Borrowed nonce base.
 * @param packageId Package id.
 * @return The nonce every block of this package uses.
 */
[[nodiscard]] std::array<std::byte, crypto::aes_gcm::kNonceSize>
package_nonce(std::span<const std::byte, crypto::aes_gcm::kNonceSize> base,
              std::uint16_t packageId) noexcept {
    std::array<std::byte, crypto::aes_gcm::kNonceSize> nonce{};
    std::copy(base.begin(), base.end(), nonce.begin());
    nonce[0] ^= static_cast<std::byte>((packageId >> 8U) & 0xFFU);
    nonce[1] = kNonceBranchByte;
    nonce[11] ^= static_cast<std::byte>(packageId & 0xFFU);
    return nonce;
}

/**
 * Loads one block, from the block cache when it is already decoded.
 * @param stem Package stem used to reach any patch file.
 * @param packageId Package id from the tag handle.
 * @param installedOnly Reads the installed patch file even when a mod serves the same patch.
 * @param record Block-table record.
 * @param keys Borrowed block keys.
 * @param nonce Package nonce.
 * @param scratch Lock-owned block storage.
 * @param plaintext Receives the view of the decoded block.
 * @return True when the block reads, authenticates and decompresses.
 */
[[nodiscard]] bool load_block(const Path& stem,
                              std::uint16_t packageId,
                              bool installedOnly,
                              const layout::BlockRecord& record,
                              const BlockKeys& keys,
                              std::span<const std::byte, crypto::aes_gcm::kNonceSize> nonce,
                              Scratch& scratch,
                              std::span<const std::byte>& plaintext) noexcept {
    const std::uint64_t blockKey = block_cache::key_of(packageId, record);
    if (block_cache::find(scratch, blockKey, plaintext)) {
        return true;
    }
    if (record.size == 0 || record.size > scratch.ciphertext.size()) {
        return false;
    }
    Path path{};
    const auto stored = std::span(scratch.ciphertext).first(record.size);
    if (!build_path(stem, packageId, record.patchId, installedOnly, path)
        || !read_at(scratch, path, record.offset, stored)) {
        return false;
    }
    std::span<const std::byte> decoded = stored;
    if ((record.flags & layout::BlockFlags::kEncrypted) != 0) {
        const auto& key =
            (record.flags & layout::BlockFlags::kAlternateKey) != 0 ? keys.alternate : keys.primary;
        const auto opened = std::span(scratch.plaintext).first(record.size);
        if (!crypto::aes_gcm::decrypt(
                std::span<const std::byte, crypto::aes_gcm::kKeySize>(key),
                nonce,
                stored,
                std::span<const std::byte, crypto::aes_gcm::kTagSize>(record.gcmTag),
                opened)) {
            return false;
        }
        decoded = opened;
    }
    if ((record.flags & layout::BlockFlags::kCompressed) == 0) {
        block_cache::store(scratch, blockKey, decoded, plaintext);
        return true;
    }
    std::size_t produced = 0;
    if (!compression::oodle::decompress_installed_block(
            decoded, std::span(scratch.decompressed).first(layout::kBlockSize), produced)) {
        return false;
    }
    block_cache::store(
        scratch, blockKey, std::span(scratch.decompressed).first(produced), plaintext);
    return true;
}

/** One tag resolved to its package file, header and entry record. */
struct LocatedEntry final {
    std::uint16_t packageId{};
    const PackageLocation* location{};
    Header header{};
    layout::EntryRecord entry{};
};

/**
 * Resolves one tag to its entry record without reading its body.
 * @param source Installed package source.
 * @param scratch Reader caches.
 * @param tag Definition tag.
 * @param output Receives the package, file, header and entry record.
 * @return True when the tag names an entry inside its package.
 */
[[nodiscard]] bool locate_entry(const Source& source,
                                Scratch& scratch,
                                std::uint32_t tag,
                                LocatedEntry& output) noexcept {
    output = {};
    if (tag < layout::kTagBase) {
        return false;
    }
    const std::uint32_t handle = tag - layout::kTagBase;
    output.packageId = static_cast<std::uint16_t>(handle >> layout::kTagEntryBits);
    const std::uint32_t entryIndex = handle & layout::kTagEntryMask;
    return resolve_latest(scratch, source, output.packageId, output.location)
           && output.location != nullptr
           && block_cache::load_header(output.location->latestPath,
                                       output.packageId,
                                       output.location->patchIndex,
                                       scratch,
                                       output.header)
           && entryIndex < output.header.entryCount
           && table_cache::entry_record(
               scratch, output.location->latestPath, output.header, entryIndex, output.entry);
}

} // namespace

/** Closes the package files the class sweeps keep open. */
void release_caches() noexcept {
    handle_cache::release();
}

/** @param scratch Reader whose own files are closed and whose held tables are dropped. */
void close_files(Scratch& scratch) noexcept {
    handle_cache::close(scratch);
    release_locations(scratch);
    scratch.packageDirectory = {};
    scratch.packageDirectoryLength = 0;
    scratch.packageLocationFallback = {};
    // Dropped slots stay chained otherwise, and the next hold would chain one slot twice.
    clear_slot_index(scratch.tableIndex);
    for (TableSlot& slot : scratch.tables) {
        slot.occupied = false;
        slot.entryCount = 0;
        slot.blockCount = 0;
        slot.used = 0;
    }
}

/** Reads one tag's entry class without decrypting or decompressing its body. */
bool read_tag_class(const Source& source,
                    Scratch& scratch,
                    std::uint32_t tag,
                    std::uint32_t& classId) noexcept {
    classId = 0;
    LocatedEntry located{};
    if (!locate_entry(source, scratch, tag, located)) {
        return false;
    }
    classId = located.entry.reference;
    return true;
}

/** Reads one tagged entry out of the installed packages. */
bool read_tag(const Source& source,
              Scratch& scratch,
              std::uint32_t tag,
              std::vector<std::byte>& output,
              std::uint32_t& classId) noexcept {
    output.clear();
    if (source.keys == nullptr) {
        return false;
    }
    classId = 0;
    LocatedEntry located{};
    if (!locate_entry(source, scratch, tag, located)) {
        return false;
    }
    classId = located.entry.reference;
    const std::uint16_t packageId = located.packageId;
    const PackageLocation* location = located.location;
    const Header& header = located.header;
    const layout::EntryPlacement placement = layout::placement(located.entry);
    if (placement.size == 0) {
        return false;
    }
    output.resize(placement.size);

    auto nonce = package_nonce(
        std::span<const std::byte, crypto::aes_gcm::kNonceSize>(source.keys->nonceBase), packageId);
    const NonceWiper nonceWiper{nonce};
    std::uint32_t blockIndex = placement.startBlock;
    std::size_t copied = 0;
    while (copied < placement.size) {
        if (blockIndex >= header.blockCount) {
            return false;
        }
        layout::BlockRecord record{};
        if (!table_cache::block_record(scratch, location->latestPath, header, blockIndex, record)) {
            return false;
        }
        std::span<const std::byte> plaintext;
        const bool decoded = load_block(location->stem,
                                        packageId,
                                        source.installedOnly,
                                        record,
                                        *source.keys,
                                        nonce,
                                        scratch,
                                        plaintext);
        if (!decoded) {
            return false;
        }
        const std::size_t skip = blockIndex == placement.startBlock ? placement.startOffset : 0;
        if (skip > plaintext.size()) {
            return false;
        }
        const std::size_t take =
            (std::min)(plaintext.size() - skip, static_cast<std::size_t>(placement.size) - copied);
        std::copy_n(plaintext.data() + skip, take, output.data() + copied);
        copied += take;
        ++blockIndex;
        if (take == 0) {
            return false;
        }
    }
    return copied == placement.size;
}

/** Reads one tagged entry without reporting its class. */
bool read_tag(const Source& source,
              Scratch& scratch,
              std::uint32_t tag,
              std::vector<std::byte>& output) noexcept {
    std::uint32_t classId = 0;
    return read_tag(source, scratch, tag, output, classId);
}

} // namespace sunrise::middleware::content::packages::reader
