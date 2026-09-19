#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../encoding/bit_reader.h"

namespace sunrise::middleware::gameplay::external {

/**
 * Channel 3 of the external frame carries the sending client's own player snapshot, reflected
 * root 0x80806AE6: the player key, then 0x80806AE9 with the world position first. A client in a
 * private activity sends it on every in-world frame.
 */
inline constexpr std::size_t kPlayerSnapshotShortCount = 64;
/** Snapshot bits after the lane-presence bit. */
inline constexpr std::size_t kPlayerSnapshotBits =
    64 + 3 * 32 + 2 * 64 + 10 + 14 + 32 + 5 + kPlayerSnapshotShortCount * 16;
static_assert(kPlayerSnapshotBits == 1373);

/** One decoded 0x80806AE6 snapshot. Only the key and position have a proven meaning. */
struct PlayerSnapshot final {
    std::uint64_t playerKey{};
    /** World position, raw IEEE-754 floats in x, y, z order. */
    std::array<float, 3> position{};
    std::uint64_t firstIdentity{};
    std::uint64_t secondIdentity{};
    std::int32_t firstValue{};
    std::int32_t secondValue{};
    std::int32_t thirdValue{};
    std::array<bool, 5> flags{};
    std::array<std::int16_t, kPlayerSnapshotShortCount> shorts{};
};

/** One channel-3 lane: an optional snapshot and its trailing-list flag. */
struct PlayerLane final {
    PlayerSnapshot snapshot{};
    bool present{};
    bool trailingList{};
};

/** Reads the complete channel-3 lane. Consumes exactly the lane's bits on success. */
[[nodiscard]] bool read_player_lane(encoding::bits::Reader& reader, PlayerLane& output) noexcept;

} // namespace sunrise::middleware::gameplay::external
