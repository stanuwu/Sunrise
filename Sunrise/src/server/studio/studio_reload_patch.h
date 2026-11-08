#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "studio_reload_manifest.h"

namespace sunrise::server::studio::reload_patch {

/** One entry's answer: an outcome and its detail code or resident crc32. */
struct Answer {
    reload_manifest::Outcome outcome{};
    std::uint32_t detail{};
};

/** One range written to a resident tag, kept so a moved tag can be written again. */
struct Written {
    std::uint32_t tag{};
    std::uint32_t offset{};
    std::uint32_t oldCrc32{};
    std::vector<std::byte> bytes;
};

/**
 * Writes one entry's patch ranges into the tag the client holds. Every guard is checked before
 * any byte is written, and no range may leave the resident tag or the entry's size.
 * @param patchBlob The manifest's patch bytes, which the entry's ranges index.
 * @param written Receives each range written, for `settle`.
 * @return applied, notResident, guardMismatch, or refused with a detail.
 */
[[nodiscard]] Answer apply(const reload_manifest::Entry& entry,
                           std::span<const std::byte> patchBlob,
                           std::vector<Written>& written) noexcept;

/**
 * Checks the written ranges after the tag heap has had time to finish moving blocks, and writes
 * a range again where a move left the old bytes. Blocks the calling thread.
 * @return Tags with a range that still lacks the new bytes after the last check.
 */
[[nodiscard]] std::vector<std::uint32_t> settle(std::span<const Written> written) noexcept;

/** @return The answer to queryResident for one tag. */
[[nodiscard]] Answer query(const reload_manifest::Entry& entry) noexcept;

/**
 * Keeps an entry's ranges so the client's next load of the tag gets them.
 * @param patchBlob The manifest's patch bytes, which the entry's ranges index.
 * @return False when the ranges are malformed or the tag holds a kept update of another size.
 */
[[nodiscard]] bool retain(const reload_manifest::Entry& entry,
                          std::span<const std::byte> patchBlob) noexcept;

} // namespace sunrise::server::studio::reload_patch
