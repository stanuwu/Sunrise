#include "player_snapshot_codec.h"

#include <bit>

#include "common_state.h"

namespace sunrise::middleware::gameplay::external {
namespace {

/** Reads one unsigned field of `width` bits and removes its reflected bias. */
[[nodiscard]] bool read_biased(encoding::bits::Reader& reader,
                               std::uint8_t width,
                               std::int64_t bias,
                               std::int32_t& output) noexcept {
    std::uint64_t value = 0;
    if (!reader.read(width, value)) {
        return false;
    }
    output = static_cast<std::int32_t>(static_cast<std::int64_t>(value) - bias);
    return true;
}

[[nodiscard]] bool read_float(encoding::bits::Reader& reader, float& output) noexcept {
    std::uint64_t value = 0;
    if (!reader.read(32, value)) {
        return false;
    }
    output = std::bit_cast<float>(static_cast<std::uint32_t>(value));
    return true;
}

/** Reads the 1373-bit 0x80806AE6 body in reflected field order. */
[[nodiscard]] bool read_snapshot(encoding::bits::Reader& reader, PlayerSnapshot& output) noexcept {
    if (!reader.read(64, output.playerKey)) {
        return false;
    }
    for (float& axis : output.position) {
        if (!read_float(reader, axis)) {
            return false;
        }
    }
    if (!reader.read(64, output.firstIdentity) || !reader.read(64, output.secondIdentity)
        || !read_biased(reader, 10, 1, output.firstValue)
        || !read_biased(reader, 14, 1, output.secondValue)
        || !read_biased(reader, 32, 0x80000000LL, output.thirdValue)) {
        return false;
    }
    for (bool& flag : output.flags) {
        if (!read_flag(reader, flag)) {
            return false;
        }
    }
    for (std::int16_t& value : output.shorts) {
        std::int32_t decoded = 0;
        if (!read_biased(reader, 16, 0x8000, decoded)) {
            return false;
        }
        value = static_cast<std::int16_t>(decoded);
    }
    return true;
}

} // namespace

bool read_player_lane(encoding::bits::Reader& reader, PlayerLane& output) noexcept {
    PlayerLane candidate{};
    encoding::bits::Reader lane = reader;
    if (!read_flag(lane, candidate.present)) {
        return false;
    }
    if (candidate.present
        && (!read_snapshot(lane, candidate.snapshot) || !read_flag(lane, candidate.trailingList))) {
        return false;
    }
    reader = lane;
    output = candidate;
    return true;
}

} // namespace sunrise::middleware::gameplay::external
