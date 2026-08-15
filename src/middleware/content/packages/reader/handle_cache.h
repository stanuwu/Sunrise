#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "internal.h"

namespace sunrise::middleware::content::packages::reader::handle_cache {

/**
 * Reads an exact byte range from one package file, keeping the file open.
 * A pass reads a package hundreds of thousands of times, and an open costs 30 times a read.
 * This cache is shared and locks, so the class sweeps use it and parallel readers must not.
 * @param path Full package path.
 * @param offset File offset.
 * @param output Exact destination.
 * @return True when every requested byte is read.
 */
[[nodiscard]] bool
read(const Path& path, std::uint64_t offset, std::span<std::byte> output) noexcept;

/**
 * Reads an exact byte range through the files one reader keeps open.
 * @param scratch The reader's own storage.
 * @param path Full package path.
 * @param offset File offset.
 * @param output Exact destination.
 * @return True when every requested byte is read.
 */
[[nodiscard]] bool read(Scratch& scratch,
                        const Path& path,
                        std::uint64_t offset,
                        std::span<std::byte> output) noexcept;

/** Closes the shared files, which the build passes do when they finish. */
void release() noexcept;

/** @param scratch Reader whose own files are closed. */
void close(Scratch& scratch) noexcept;

} // namespace sunrise::middleware::content::packages::reader::handle_cache
