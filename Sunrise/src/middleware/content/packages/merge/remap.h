#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "merge.h"

namespace sunrise::middleware::content::packages::merge::remap {

/** What one location holds; the values are the reference map's row kinds. */
enum class Kind : std::uint8_t {
    tag = 1,
    sidecarTag = 3,
    package = 5,
};

/** Entry index that names a package's misc region, and one that names its merge sidecar. */
inline constexpr std::uint32_t kMiscEntry = 0xFFFFFFFFU, kSidecarEntry = 0xFFFFFFFEU;

/** One location in a package family: an entry offset, a misc offset or a sidecar offset. */
struct Location final {
    std::uint16_t package{};
    std::uint32_t entry{};
    std::uint32_t offset{};
    Kind kind{};
};

/** One package id a mod gives up, and the one it takes. */
struct Move final {
    std::uint16_t from{};
    std::uint16_t to{};
};

/** One package file and the path of its rewritten copy; a sidecar follows beside both. */
struct File final {
    std::wstring source;
    std::wstring output;
};

/**
 * Copies every file with each location's value moved, the block and region digests recomputed
 * and moved headers renumbered. An identical existing copy is kept, so a file the client holds
 * open stays valid.
 * @param files Every generation of each family a location names, plus any other file to copy.
 * @return ok; format when a location does not fit its file; unreadable or writeFailed on I/O.
 */
[[nodiscard]] Status rewrite(std::span<const File> files,
                             std::span<const Location> locations,
                             std::span<const Move> moves) noexcept;

/**
 * Moves one value in place.
 * @param bytes Two bytes for a package id, four for a tag.
 * @param changed Set when the value moved.
 * @return False when a tag location does not hold a tag.
 */
[[nodiscard]] bool move_value(std::span<std::byte> bytes,
                              Kind kind,
                              std::span<const Move> moves,
                              bool& changed) noexcept;

} // namespace sunrise::middleware::content::packages::merge::remap
