#pragma once

#include <cstdint>
#include <span>

#include "internal.h"
#include "layout.h"
#include "reader.h"

namespace sunrise::middleware::content::packages::reader::block_cache {

/**
 * Reports the cached copy of one block.
 * @param scratch Lock-owned block storage.
 * @param key Block cache key.
 * @param plaintext Receives the view of the cached block.
 * @return True when the block is cached.
 */
[[nodiscard]] bool
find(Scratch& scratch, std::uint64_t key, std::span<const std::byte>& plaintext) noexcept;

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
           std::span<const std::byte>& plaintext) noexcept;

/**
 * Builds the cache key of one package block.
 * @param packageId Package id from the tag handle.
 * @param record Block-table record.
 * @return A value unique to that block of that package patch.
 */
[[nodiscard]] std::uint64_t key_of(std::uint16_t packageId,
                                   const layout::BlockRecord& record) noexcept;

/**
 * Reads one package header, from the header cache when it is already parsed.
 * @param path Full package path.
 * @param packageId Package id from the tag handle.
 * @param patchIndex Patch index the path names.
 * @param scratch Lock-owned block storage.
 * @param header Receives the header fields.
 * @return True when the header reads and parses.
 */
[[nodiscard]] bool load_header(const Path& path,
                               std::uint16_t packageId,
                               std::uint32_t patchIndex,
                               Scratch& scratch,
                               Header& header) noexcept;

} // namespace sunrise::middleware::content::packages::reader::block_cache
