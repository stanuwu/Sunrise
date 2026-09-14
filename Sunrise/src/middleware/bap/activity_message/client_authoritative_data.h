#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../encoding/bit_reader.h"
#include "transport_report.h"

namespace sunrise::middleware::bap::activity_message::client_authoritative_data {

/** The 3-bit state field decodes from wire zero to logical -1. */
inline constexpr std::int8_t kMinimumState = -1;
/** The 3-bit state field reaches logical 6 after the bias is removed. */
inline constexpr std::int8_t kMaximumState = 6;
/** Wire zero names no teleport slice set after the bias is removed. */
inline constexpr std::int32_t kAbsentSliceSetIndex = -1;
/** The 10-bit teleport slice-set field reaches logical 1022. */
inline constexpr std::int32_t kMaximumSliceSetIndex = 1022;
/** Logical -1 names no region; D6 field 2 encodes it as wire 0x7FFFFFFF. */
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
    /** Spawn set the move leaves as the client's spawn-point filter, not a slice-set name. */
    std::uint32_t spawnSetHash{};
};

/** Wire zero names no slice set after the 10-bit leg field's bias is removed. */
inline constexpr std::int32_t kAbsentLegSliceSetIndex = -1;
/** The two 2-bit leg fields decode from wire zero to logical -1 and reach 2. */
inline constexpr std::int8_t kMinimumLegState = -1;
inline constexpr std::int8_t kMaximumLegState = 2;

/**
 * One D6 region leg as the client reports it. The host mirrors these fields back into the
 * client's own msg-12 member record, so they are kept exactly as sent.
 */
struct RegionState final {
    std::int32_t index{kAbsentRegionIndex};
    std::uint32_t hash{};
    std::int32_t sliceSetIndex{kAbsentLegSliceSetIndex};
    /** Field 3, the region's `PUB` or `PRV` policy as the client formats it. */
    std::int8_t publicState{kMinimumLegState};
    /** Field 4, stored and copied by the client; no reader is known. */
    std::int8_t auxState{kMinimumLegState};
    bool hasHash{};
};

/**
 * Typed client-reported state from one sparse delta; this does not grant mission authority.
 * `currentRegion` is the region of the slice set the client holds, -1 while none is instantiated.
 * `region` is the pending leg: what it is loading, or what is behind it after a z-leg switch.
 */
struct ClientAuthoritativeData final {
    TransportReport transport{};
    SpawnState spawn{};
    TeleportState teleport{};
    RegionState currentRegion{};
    RegionState region{};
    std::uint8_t transitionToken{};
    bool hasTransitionToken{};
    bool hasSpawn{};
    bool hasTeleport{};
    bool hasCurrentRegion{};
    bool hasRegion{};
};

/** Client-reported state deltas use activity message type 22; the host remains authoritative. */
inline constexpr std::uint32_t kMessageType = 22;
/** The root presence bit needs at least one padded byte. */
inline constexpr std::size_t kMinimumEncodedSize = 1;
/** The largest valid schema walk carries 86,661 meaningful bits. */
inline constexpr std::size_t kMaximumMeaningfulBitCount = 86'661;
/** The largest valid schema walk ends in 10,833 bytes. */
inline constexpr std::size_t kMaximumEncodedSize = 10'833;

/**
 * Parses one sparse client-state delta and keeps only the reflected typed report. This wire input
 * does not grant mission authority.
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
/** Reads the native transport fields while validating the entire B2 branch. */
[[nodiscard]] bool read_transport_branch(encoding::bits::Reader& reader,
                                         TransportReport& report) noexcept;

/**
 * Reads the D4 branch and keeps its optional transition token and both region legs.
 * @param reader Reader sitting at the first D4 field.
 * @param update Candidate result that receives the present legs and token.
 * @return True when both legs and all scalar fields fit.
 */
[[nodiscard]] bool read_transition_branch(encoding::bits::Reader& reader,
                                          ClientAuthoritativeData& update) noexcept;

} // namespace sunrise::middleware::bap::activity_message::client_authoritative_data
