#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>

#include "../schema/generated/tag_classes_808091.h"
#include "definition_index_table.h"

namespace sunrise::middleware::content::packages::tables {

/** Tag class of a spawn-point set. */
inline constexpr std::uint32_t kSpawnSetClass = schema::generated::SSpawnPoints::kClass;
/** A spawn set holds its point array descriptor at this offset. */
inline constexpr std::size_t kSpawnSetDescriptor =
    offsetof(schema::generated::SSpawnPoints, spawnPoints);
/** Element class of a spawn set's point array. */
inline constexpr std::uint32_t kSpawnPointClass = schema::generated::SSpawnPointRow::kClass;
inline constexpr std::size_t kSpawnPointStride = sizeof(schema::generated::SSpawnPointRow);
/** A spawn point holds its rotation quaternion at this offset. */
inline constexpr std::size_t kSpawnPointRotationOffset =
    offsetof(schema::generated::SSpawnPointRow, rotation);
/** A spawn point holds its position at this offset. */
inline constexpr std::size_t kSpawnPointPositionOffset =
    offsetof(schema::generated::SSpawnPointRow, position);
/** A spawn point holds its set-name hash at this offset. */
inline constexpr std::size_t kSpawnPointNameHashOffset =
    offsetof(schema::generated::SSpawnPointRow, spawnNameIdentity);
/** A rotation is four floats. */
inline constexpr std::size_t kRotationComponents =
    std::tuple_size_v<decltype(schema::generated::SSpawnPointRow::rotation)>;
/** A position is three floats. */
inline constexpr std::size_t kPositionComponents =
    std::tuple_size_v<decltype(schema::generated::SSpawnPointRow::position)>;
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
