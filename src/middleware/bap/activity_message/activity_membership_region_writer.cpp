#include "replicate_membership.h"

namespace sunrise::middleware::bap::activity_message::replicate_membership {
namespace {

/** The host table has one fixed record for each of 64 bubbles. */
constexpr std::size_t kRegionCount = 64;
/** Each bubble's state-zero slice-set index is 8 times its bubble index. */
constexpr std::uint32_t kRegionIndexStride = 8;
/** Region indices use the signed 32-bit midpoint as their wire bias. */
constexpr std::uint32_t kRegionIndexBias = 0x80000000U;
/** Each record carries one transition-token byte for every member slot. */
constexpr std::size_t kRegionTokenCount = 32;
/** Spawn and teleport state fields use 3 bits. */
constexpr std::uint8_t kStateBitWidth = 3;
/** Teleport slice-set indices use 10 bits. */
constexpr std::uint8_t kSliceSetBitWidth = 10;
/** Signed host-state fields add 1 before writing their unsigned value. */
constexpr std::int32_t kSignedFieldBias = 1;

/**
 * Writes one 451-bit state-zero region record.
 * @param writer Fixed-buffer writer sitting at the record start.
 * @param bubble Bubble index, 0 through 63.
 * @param token Transition token copied into all member lanes.
 * @return True when every fixed field fits.
 */
[[nodiscard]] bool
write_region(encoding::bits::Writer& writer, std::size_t bubble, std::uint8_t token) noexcept {
    const std::uint32_t region = static_cast<std::uint32_t>(bubble) * kRegionIndexStride;
    bool encoded = writer.write(kRegionIndexBias + region, 32) && writer.write(1, 2)
                   && writer.write(0, 8) && writer.write(2, 2) && writer.write(1, 6)
                   && writer.write(0, 1) && writer.write(0, 32) && writer.write(0, 8)
                   && writer.write(0, 32);
    for (std::size_t member = 0; encoded && member < kRegionTokenCount; ++member) {
        encoded = writer.write(token, 8);
    }
    return encoded && writer.write(0, 8) && writer.write(0, 64);
}

/**
 * Writes one host key, low byte first.
 * @param writer Fixed-buffer writer sitting after the host presence bit.
 * @param key Host-order key shared with the populated member.
 * @return True when all 8 key bytes fit.
 */
[[nodiscard]] bool write_host_key(encoding::bits::Writer& writer, std::uint64_t key) noexcept {
    for (std::size_t index = 0; index < sizeof key; ++index) {
        if (!writer.write((key >> (index * 8U)) & 0xFFU, 8)) {
            return false;
        }
    }
    return true;
}

/**
 * Writes the current host-state tail after all region records.
 * @param writer Fixed-buffer writer sitting at the first spawn field.
 * @param snapshot Reflected host key, spawn state and teleport state.
 * @return True when the host-present tail fits.
 */
[[nodiscard]] bool write_host_tail(encoding::bits::Writer& writer,
                                   const MembershipSnapshot& snapshot) noexcept {
    // Both state fields are 3 bits at bias 1, so a logical -1 encodes as wire zero, which is fatal
    // in each: the spawn reader then refuses every spawn, and the teleport reader arms a teleport
    // every cycle and de-instantiates the world. So a negative mirror is sent as a neutral zero.
    const auto spawnState = static_cast<std::uint8_t>(
        (snapshot.spawn.state < 0 ? 0 : snapshot.spawn.state) + kSignedFieldBias);
    const auto teleportState = static_cast<std::uint8_t>(
        (snapshot.teleport.state < 0 ? 0 : snapshot.teleport.state) + kSignedFieldBias);
    const auto sliceSetIndex =
        static_cast<std::uint32_t>(snapshot.teleport.sliceSetIndex + kSignedFieldBias);
    return writer.write(snapshot.spawn.opaqueByte, 8) && writer.write(spawnState, kStateBitWidth)
           && writer.write(snapshot.spawn.opaqueValue, 64) && writer.write(0, 4)
           && writer.write(teleportState, kStateBitWidth)
           && writer.write(snapshot.teleport.token, 8)
           && writer.write(sliceSetIndex, kSliceSetBitWidth)
           && writer.write(snapshot.teleport.sliceSetHash, 32) && writer.write(0, 1)
           && writer.write(1, 1) && write_host_key(writer, snapshot.identity.memberKey)
           && writer.write(0, 1) && writer.write(0, 1);
}

} // namespace

/** Writes all 64 state-zero regions and the host-present tail. */
bool write_region_block(encoding::bits::Writer& writer,
                        const MembershipSnapshot& snapshot) noexcept {
    bool encoded = writer.bit_count() == kRegionBlockStartBit;
    for (std::size_t bubble = 0; encoded && bubble < kRegionCount; ++bubble) {
        encoded = write_region(writer, bubble, snapshot.transitionToken);
    }
    return encoded && write_host_tail(writer, snapshot) && writer.bit_count() == kRegionBlockEndBit;
}

} // namespace sunrise::middleware::bap::activity_message::replicate_membership
