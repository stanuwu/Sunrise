#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../encoding/bit_reader.h"

namespace sunrise::middleware::bap::activity_message::client_authoritative_data {

/** The 3-bit state field decodes from wire zero to logical -1. */
inline constexpr std::int8_t kMinimumState = -1;
/** The 3-bit state field reaches logical 6 after the bias is removed. */
inline constexpr std::int8_t kMaximumState = 6;
/** Wire zero names no teleport slice set after the bias is removed. */
inline constexpr std::int32_t kAbsentSliceSetIndex = -1;
/** The 10-bit teleport slice-set field reaches logical 1022. */
inline constexpr std::int32_t kMaximumSliceSetIndex = 1022;
/** Wire zero names no region once the bias is removed. The client sends it on its way out. */
inline constexpr std::int32_t kAbsentRegionIndex = -1;

/** Complete spawn-state snapshot mirrored between activity messages 22 and 12. */
struct SpawnState final {
    std::int8_t state{};
    std::uint8_t opaqueByte{};
    std::uint64_t opaqueValue{};
};

/** Teleport-state fields mirrored between activity messages 22 and 12. */
struct TeleportState final {
    std::int8_t state{};
    std::uint8_t token{};
    std::int32_t sliceSetIndex{kAbsentSliceSetIndex};
    std::uint32_t sliceSetHash{};
};

/**
 * The region the client says it is in, and that region's name hash. It moves as the player
 * crosses a bubble boundary, so it beats the destination's own slice set everywhere the host has
 * to name the player's position.
 */
struct RegionState final {
    std::int32_t index{kAbsentRegionIndex};
    std::uint32_t hash{};
    bool hasHash{};
};

/** Typed changes kept from one sparse client-authoritative delta. */
struct ClientAuthoritativeData final {
    SpawnState spawn{};
    TeleportState teleport{};
    RegionState region{};
    std::uint8_t transitionToken{};
    bool hasTransitionToken{};
    bool hasSpawn{};
    bool hasTeleport{};
    bool hasRegion{};
};

/** Client-authoritative deltas use activity message type 22. */
inline constexpr std::uint32_t kMessageType = 22;
/** The root presence bit needs at least one padded byte. */
inline constexpr std::size_t kMinimumEncodedSize = 1;
/** The largest valid schema walk carries 86,661 meaningful bits. */
inline constexpr std::size_t kMaximumMeaningfulBitCount = 86'661;
/** The largest valid schema walk ends in 10,833 bytes. */
inline constexpr std::size_t kMaximumEncodedSize = 10'833;

/**
 * Parses one sparse authoritative delta and keeps only the reflected typed state.
 * @param input Exact message-22 body, with zero padding in its last byte.
 * @param update Cleared first. Filled in only on success.
 * @return True when the whole schema walk uses up the exact padded body.
 */
[[nodiscard]] bool parse_client_authoritative_data(std::span<const std::byte> input,
                                                   ClientAuthoritativeData& update) noexcept;

/**
 * Reads one optional-field marker.
 * @param reader Reader sitting at the field marker.
 * @param present Receives true only when the nested field follows.
 * @return True when the marker was there.
 */
[[nodiscard]] bool read_presence(encoding::bits::Reader& reader, bool& present) noexcept;

/**
 * Skips the whole opaque B2 branch and checks its dynamic count.
 * @param reader Reader sitting at the first B2 field.
 * @return True when every present field and the required tail fit.
 */
[[nodiscard]] bool skip_opaque_root_branch(encoding::bits::Reader& reader) noexcept;

/**
 * Reads the D4 branch and keeps only its optional transition token.
 * @param reader Reader sitting at the first D4 field.
 * @param update Candidate result that receives a present token.
 * @return True when both opaque legs and all scalar fields fit.
 */
[[nodiscard]] bool read_transition_branch(encoding::bits::Reader& reader,
                                          ClientAuthoritativeData& update) noexcept;

} // namespace sunrise::middleware::bap::activity_message::client_authoritative_data
