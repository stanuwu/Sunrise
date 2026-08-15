#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "definition_index_table.h"

namespace sunrise::middleware::content::packages::tables {

/** Tag class of a spawn-point set. */
inline constexpr std::uint32_t kSpawnSetClass = 0x80809162U;
/** A spawn set holds its point array descriptor at this offset. */
inline constexpr std::size_t kSpawnSetDescriptor = 8;
/** Element class of a spawn set's point array. */
inline constexpr std::uint32_t kSpawnPointClass = 0x80809164U;
/** One spawn point is 48 inline bytes. */
inline constexpr std::size_t kSpawnPointStride = 48;
/** A spawn point holds its rotation quaternion at this offset. */
inline constexpr std::size_t kSpawnPointRotationOffset = 0;
/** A spawn point holds its position at this offset. */
inline constexpr std::size_t kSpawnPointPositionOffset = 16;
/** A spawn point holds its set-name hash at this offset. */
inline constexpr std::size_t kSpawnPointNameHashOffset = 32;
/** A rotation is four floats. */
inline constexpr std::size_t kRotationComponents = 4;
/** A position is three floats. */
inline constexpr std::size_t kPositionComponents = 3;
/** The name hash of the set ordinary arrivals use. */
inline constexpr std::uint32_t kDefaultSpawnNameHash = 0x2EA8FB98U;
/** The hash an unnamed spawn point carries. */
inline constexpr std::uint32_t kUnnamedSpawnNameHash = 0x811C9DC5U;

/** One spawn point of a spawn-point set. */
struct SpawnPoint {
    std::array<float, kRotationComponents> rotation{};
    std::array<float, kPositionComponents> position{};
    std::uint32_t nameHash{};
};

/**
 * Locates the point array of one spawn set.
 * @param blob Complete spawn-set blob.
 * @param output Cleared before validation and filled only on success.
 * @return True when the descriptor resolves to a valid array header.
 */
[[nodiscard]] bool spawn_points(std::span<const std::byte> blob, Array& output) noexcept;

/**
 * Reads one spawn point out of a located point array.
 * @param blob Complete spawn-set blob.
 * @param array Located point array.
 * @param index Point ordinal inside the array.
 * @param output Cleared before validation and filled only on success.
 * @return True when the rotation, position, and name hash all read back.
 */
[[nodiscard]] bool spawn_point_at(std::span<const std::byte> blob,
                                  const Array& array,
                                  std::size_t index,
                                  SpawnPoint& output) noexcept;

} // namespace sunrise::middleware::content::packages::tables
