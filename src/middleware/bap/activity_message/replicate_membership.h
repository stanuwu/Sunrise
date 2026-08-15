#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../encoding/bit_writer.h"
#include "activity_client_identity_parser.h"
#include "client_authoritative_data.h"

namespace sunrise::middleware::bap::activity_message::replicate_membership {

/** Membership snapshots use activity message type 12. */
inline constexpr std::uint32_t kMessageType = 12;
/** One local player plus a reflected host is 29,968 meaningful bits. */
inline constexpr std::size_t kMeaningfulBitCount = 29'968;
/** The host-present snapshot is byte-aligned at 3,746 bytes. */
inline constexpr std::size_t kEncodedSize = 3'746;

/** Inputs for one local-player membership snapshot. */
struct MembershipSnapshot final {
    client_identity::ClientIdentity identity{};
    client_authoritative_data::SpawnState spawn{};
    client_authoritative_data::TeleportState teleport{};
    std::uint32_t revision{};
    /** Stable session epoch; changing it clears the client's peer table. */
    std::uint32_t epoch{};
    /** Transition token copied into every member lane of every region. */
    std::uint8_t transitionToken{};
};

/**
 * Encodes one full-player membership snapshot. No allocation.
 * @param snapshot Checked identity, revision, transition, and host-echo values.
 * @param output Caller storage, left unchanged when validation fails or it is too small.
 * @param written Receives 3,746 on success or zero on failure.
 * @return True when the host-present body was encoded.
 */
[[nodiscard]] bool encode_replicate_membership(const MembershipSnapshot& snapshot,
                                               std::span<std::byte> output,
                                               std::size_t& written) noexcept;

/** The local member begins after root, revision, and epoch fields. */
inline constexpr std::size_t kMemberStartBit = 65;
/** The full identity shifts the region block to bit 835. */
inline constexpr std::size_t kRegionBlockStartBit = 835;
/** The host-present region block ends before top-level field four. */
inline constexpr std::size_t kRegionBlockEndBit = 29'899;

/** @return True when the teleport slice-set index fits its fixed wire field. */
[[nodiscard]] bool valid(const MembershipSnapshot& snapshot) noexcept;

/**
 * Writes the one populated member and 31 absent member slots.
 * @param writer Fixed-buffer writer positioned at bit 65.
 * @param identity Exact client identity accepted for the current join.
 * @return True when the writer reaches the region-block presence bit.
 */
[[nodiscard]] bool write_member_table(encoding::bits::Writer& writer,
                                      const client_identity::ClientIdentity& identity) noexcept;

/**
 * Writes all 64 state-zero regions and the host-present tail.
 * @param writer Fixed-buffer writer positioned at bit 835.
 * @param snapshot Transition token and reflected host state.
 * @return True when the writer reaches top-level field four.
 */
[[nodiscard]] bool write_region_block(encoding::bits::Writer& writer,
                                      const MembershipSnapshot& snapshot) noexcept;

} // namespace sunrise::middleware::bap::activity_message::replicate_membership
