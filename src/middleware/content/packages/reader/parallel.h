#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "reader.h"

namespace sunrise::middleware::content::packages::reader::parallel {

/** Readers one batch may run at once, whatever the machine reports. */
inline constexpr std::size_t kMaxWorkers = 4;

/**
 * Called on the reader's own thread for one tag that read.
 * The reader index picks the caller's storage, and no two readers are ever given the same one,
 * so a visitor that only touches storage chosen that way needs no lock.
 * @param context Caller context.
 * @param worker Reader index, below the count the batch reported.
 * @param tag Tag that read.
 * @param blob Entry bytes, valid only for this call.
 */
using Visitor = void (*)(void* context,
                         std::size_t worker,
                         std::uint32_t tag,
                         std::span<const std::byte> blob) noexcept;

/**
 * Reports how many readers a batch runs.
 * A block read waits out the disk before it decodes, so the readers are there to overlap that
 * wait. The count leaves cores for the game and never exceeds the batch maximum.
 * @return Reader count, at least one.
 */
[[nodiscard]] std::size_t worker_count() noexcept;

/**
 * Reads a run of tags across several readers and returns when every one has finished.
 * Each reader owns its files, block cache and buffer, so nothing is shared and nothing locks.
 * The calling thread reads the first share, then waits. Tags that will not read are skipped.
 * @param source Package directory and borrowed block keys.
 * @param tags Tags to read, in order.
 * @param visitor Required consumer, called on the reader thread.
 * @param context Caller context passed to the visitor.
 * @return True when the batch ran. False means the reader storage could not be prepared.
 */
[[nodiscard]] bool read_tags(const Source& source,
                             std::span<const std::uint32_t> tags,
                             Visitor visitor,
                             void* context) noexcept;

/** One blob a batch kept, valid until the next batch. */
struct Held {
    std::uint32_t tag{};
    std::span<const std::byte> blob;
};

/**
 * Reads a run of tags across several readers and keeps every blob that read.
 * Use it when parsing folds into one shared accumulator: readers only read, the caller parses
 * later on its own thread. Rows come back in the order the tags were given.
 * @param source Package directory and borrowed block keys.
 * @param tags Tags to read, in order.
 * @param kept Receives one row per tag that read. Its blobs live until the next call.
 * @return True when the batch ran.
 */
[[nodiscard]] bool read_kept(const Source& source,
                             std::span<const std::uint32_t> tags,
                             std::vector<Held>& kept) noexcept;

/** Closes every reader's files and frees the batch storage. */
void release() noexcept;

} // namespace sunrise::middleware::content::packages::reader::parallel
