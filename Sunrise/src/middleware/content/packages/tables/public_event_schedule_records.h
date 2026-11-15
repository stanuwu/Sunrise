#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "field_reader.h"

namespace sunrise::middleware::content::packages::tables::public_event_schedule::records {
/** The client reads the table rows and the group list through these blocks of the payload. */
inline constexpr std::size_t kRowsBlockOffset = 0x68, kGroupsBlockOffset = 0x78;
/** A block's self-relative target starts with a 16-byte array header before its rows. */
inline constexpr std::size_t kArrayHeaderBytes = 16;
/** The authored minutes become seconds through one float32 multiply, truncated. */
inline constexpr float kSecondsPerMinute = 60.0f;

#pragma pack(push, 1)
/** A tag block: a row count, then an offset relative to the offset field's own address. */
struct Block {
    std::int64_t count;
    std::int64_t relative;
};

/** One 96-byte schedule row as authored. */
struct RowRecord {
    std::uint8_t head[0x08];
    std::uint32_t key;
    std::uint8_t unknown0c[0x18];
    float periodMinutes;
    float spanMinutes;
    float offsetMinutes;
    float windowMinutes;
    std::uint8_t unknown34[0x04];
    Block entries;
    std::uint32_t salt;
    std::uint8_t unknown4c[0x04];
    Block second;
};

/** One 40-byte entry as authored. */
struct EntryRecord {
    float weight;
    std::uint8_t unknown04[0x08];
    std::uint32_t gate;
    std::uint32_t group;
    std::uint32_t site;
    std::uint32_t unknown18;
    std::uint16_t node;
    std::uint8_t unknown1e[0x02];
    std::uint32_t rally;
    std::uint8_t unknown24[0x04];
};
#pragma pack(pop)
static_assert(sizeof(Block) == 16 && sizeof(RowRecord) == 96 && sizeof(EntryRecord) == 40);

/** Resolves a block to its first row; false when the block or its rows leave the payload. */
[[nodiscard]] inline bool block_rows(std::span<const std::byte> payload,
                                     std::size_t blockOffset,
                                     std::size_t rowBytes,
                                     std::size_t& count,
                                     std::size_t& first) noexcept {
    Block block{};
    if (rowBytes == 0 || !read(payload, blockOffset, block) || block.count < 0) {
        return false;
    }
    std::size_t target{};
    // Zero deltas remain valid here; field_reader::relative intentionally rejects them.
    if (!relative_offset(blockOffset + offsetof(Block, relative), block.relative, target)
        || target > payload.size() || payload.size() - target < kArrayHeaderBytes) {
        return false;
    }
    count = static_cast<std::size_t>(block.count);
    first = target + kArrayHeaderBytes;
    return count == 0 || (payload.size() - first) / rowBytes >= count;
}

/** float32 minutes times 60, truncated toward zero, as the client's mulss and cvttss2si do. */
[[nodiscard]] inline std::int64_t seconds(float minutes) noexcept {
    const float value = minutes * kSecondsPerMinute;
    // INT64_MAX rounds up in float32; the upper bound must be exclusive.
    if (!std::isfinite(value) || value < 0.0f
        || value >= static_cast<float>((std::numeric_limits<std::int64_t>::max)())) {
        return -1;
    }
    return static_cast<std::int64_t>(value);
}

} // namespace sunrise::middleware::content::packages::tables::public_event_schedule::records
