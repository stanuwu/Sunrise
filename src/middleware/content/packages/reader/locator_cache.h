#pragma once

#include <cstdint>
#include <string_view>

#include "internal.h"

namespace sunrise::middleware::content::packages::reader::locator_cache {

/** @param value Directory text. @return Its case-sensitive key. */
[[nodiscard]] std::uint64_t directory_hash(std::wstring_view value) noexcept;

/**
 * Reads one remembered lookup.
 * @param directoryHash Key of the directory being searched.
 * @param packageId Package id being looked up.
 * @param stem Receives the remembered stem.
 * @param patchIndex Receives the remembered highest patch index.
 * @return True when this directory and package were looked up before.
 */
[[nodiscard]] bool find(std::uint64_t directoryHash,
                        std::uint16_t packageId,
                        Path& stem,
                        std::uint32_t& patchIndex) noexcept;

/**
 * Remembers one lookup, keeping the highest patch of each package.
 * @param directoryHash Key of the directory that was searched.
 * @param packageId Package id that was looked up.
 * @param stem Stem that was found.
 * @param patchIndex Patch index that was found.
 */
void store(std::uint64_t directoryHash,
           std::uint16_t packageId,
           const Path& stem,
           std::uint32_t patchIndex) noexcept;

/**
 * Reports whether one directory has been read in full.
 * Such a directory answers a miss with no second pass and never writes again, so readers take
 * the lookup lock in shared mode only.
 * @param directoryHash Key of the directory.
 * @return True when every package file of it has been recorded.
 */
[[nodiscard]] bool complete(std::uint64_t directoryHash) noexcept;

/** @param directoryHash Key of the directory whose package files are now all recorded. */
void mark_complete(std::uint64_t directoryHash) noexcept;

} // namespace sunrise::middleware::content::packages::reader::locator_cache
