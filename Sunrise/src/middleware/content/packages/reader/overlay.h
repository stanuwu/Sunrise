#pragma once

#include <cstdint>
#include <span>

#include "reader.h"

namespace sunrise::middleware::content::packages::reader::overlay {

/** One package generation a mod serves: a patch of an installed package, or a new package's. */
struct File final {
    std::uint16_t packageId{};
    std::uint32_t patchIndex{};
    /** Absolute path of the served file. */
    Path path{};
};

/**
 * Publishes the mod-served generations as a new table. Readers read the current table without a
 * lock, so this may run while they read; a reader sees the old table or the new one, whole.
 * Every table stays alive until clear, so spans from files stay valid.
 * @param files One row per served file.
 * @return False when two rows name one generation or storage failed; nothing is published then.
 */
[[nodiscard]] bool publish(std::span<const File> files) noexcept;

/** Drops every table. Runs only when no reader runs. */
void clear() noexcept;

/** @return A count that changes on every publish and clear; readers then drop cached locations. */
[[nodiscard]] std::uint64_t epoch() noexcept;

/** @return True with the highest generation a mod serves for this package. */
[[nodiscard]] bool latest(std::uint16_t packageId, std::uint32_t& patchIndex) noexcept;

/** @return True with the path of one mod-served generation. */
[[nodiscard]] bool find(std::uint16_t packageId, std::uint32_t patchIndex, Path& path) noexcept;

/** @return Every served file, ordered by package id, then patch. */
[[nodiscard]] std::span<const File> files() noexcept;

} // namespace sunrise::middleware::content::packages::reader::overlay
