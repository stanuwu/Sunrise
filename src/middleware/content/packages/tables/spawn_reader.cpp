#include "spawn_reader.h"

#include "internal.h"

namespace sunrise::middleware::content::packages::tables {

/** Locates the point array of one spawn set. */
bool spawn_points(std::span<const std::byte> blob, Array& output) noexcept {
    output = {};
    return find_array_at(blob, kSpawnSetDescriptor, output);
}

/** Reads one spawn point out of a located point array. */
bool spawn_point_at(std::span<const std::byte> blob,
                    const Array& array,
                    std::size_t index,
                    SpawnPoint& output) noexcept {
    output = {};
    std::size_t offset = 0;
    if (!element_offset(array.dataOffset, array.count, kSpawnPointStride, index, offset)) {
        return false;
    }
    for (std::size_t axis = 0; axis < kRotationComponents; ++axis) {
        const std::size_t at = offset + kSpawnPointRotationOffset + (axis * sizeof(float));
        if (!read<float>(blob, at, output.rotation[axis])) {
            output = {};
            return false;
        }
    }
    for (std::size_t axis = 0; axis < kPositionComponents; ++axis) {
        const std::size_t at = offset + kSpawnPointPositionOffset + (axis * sizeof(float));
        if (!read<float>(blob, at, output.position[axis])) {
            output = {};
            return false;
        }
    }
    if (!read<std::uint32_t>(blob, offset + kSpawnPointNameHashOffset, output.nameHash)) {
        output = {};
        return false;
    }
    return true;
}

} // namespace sunrise::middleware::content::packages::tables
