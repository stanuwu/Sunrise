#include <algorithm>
#include <array>

#include "block_cache.h"

namespace sunrise::middleware::content::packages::reader::block_cache {

/**
 * Builds the cache key of one package block.
 * @param packageId Package id from the tag handle.
 * @param record Block-table record.
 * @return A value unique to that block of that package patch.
 */
std::uint64_t key_of(std::uint16_t packageId, const layout::BlockRecord& record) noexcept {
    return (static_cast<std::uint64_t>(packageId) << 48U)
           | (static_cast<std::uint64_t>(record.patchId) << 32U) | record.offset;
}

/**
 * Reports the cached copy of one block.
 * @param scratch Lock-owned block storage.
 * @param key Block cache key.
 * @param plaintext Receives the view of the cached block.
 * @return True when the block is cached.
 */
bool find(Scratch& scratch, std::uint64_t key, std::span<const std::byte>& plaintext) noexcept {
    for (BlockSlot& slot : scratch.blocks) {
        if (slot.valid && slot.key == key) {
            slot.used = ++scratch.useCounter;
            plaintext = std::span<const std::byte>(slot.bytes.data(), slot.size);
            return true;
        }
    }
    return false;
}

/**
 * Keeps one decoded block and reports the kept copy.
 * @param scratch Lock-owned block storage.
 * @param key Block cache key.
 * @param decoded Whole decoded block.
 * @param plaintext Receives the view of the kept block.
 */
void store(Scratch& scratch,
           std::uint64_t key,
           std::span<const std::byte> decoded,
           std::span<const std::byte>& plaintext) noexcept {
    plaintext = decoded;
    BlockSlot* target = &scratch.blocks[0];
    for (BlockSlot& slot : scratch.blocks) {
        if (!slot.valid) {
            target = &slot;
            break;
        }
        if (slot.used < target->used) {
            target = &slot;
        }
    }
    if (decoded.size() > target->bytes.size()) {
        return;
    }
    std::copy(decoded.begin(), decoded.end(), target->bytes.begin());
    target->size = decoded.size();
    target->key = key;
    target->used = ++scratch.useCounter;
    target->valid = true;
    plaintext = std::span<const std::byte>(target->bytes.data(), target->size);
}

/**
 * Reads one package header, from the header cache when it is already parsed.
 * @param path Full package path.
 * @param packageId Package id from the tag handle.
 * @param patchIndex Patch index the path names.
 * @param scratch Lock-owned block storage.
 * @param header Receives the header fields.
 * @return True when the header reads and parses.
 */
bool load_header(const Path& path,
                 std::uint16_t packageId,
                 std::uint32_t patchIndex,
                 Scratch& scratch,
                 Header& header) noexcept {
    const std::uint64_t key = (static_cast<std::uint64_t>(packageId) << 32U) | patchIndex;
    for (HeaderSlot& slot : scratch.headers) {
        if (slot.valid && slot.key == key) {
            header.entryCount = slot.entryCount;
            header.blockCount = slot.blockCount;
            header.entryTable = slot.entryTable;
            header.blockTable = slot.blockTable;
            return true;
        }
    }
    std::array<std::byte, layout::kHeaderSize> headerBytes{};
    if (!read_at(scratch, path, 0, headerBytes) || !parse_header(headerBytes, header)) {
        return false;
    }
    HeaderSlot& slot = scratch.headers[key % scratch.headers.size()];
    slot.key = key;
    slot.entryCount = header.entryCount;
    slot.blockCount = header.blockCount;
    slot.entryTable = header.entryTable;
    slot.blockTable = header.blockTable;
    slot.valid = true;
    return true;
}

} // namespace sunrise::middleware::content::packages::reader::block_cache
