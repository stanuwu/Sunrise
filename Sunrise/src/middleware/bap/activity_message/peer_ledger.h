#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::activity_message::peer_ledger {

/** The client returns one peer reservation it no longer needs. */
inline constexpr std::uint32_t kReleaseReservationType = 14;
/** The client reports that one peer is leaving. */
inline constexpr std::uint32_t kPeerLeaveType = 15;
/** The client reports that it cannot reach one peer. Both sides can send this type. */
inline constexpr std::uint32_t kConnectivityFailureType = 37;
/** The client proposes a host migration. Nothing here acts on one. */
inline constexpr std::uint32_t kSpeculativeMigrationType = 48;

/** Release carries two scalars and a peer key in exactly 16 bytes. */
inline constexpr std::size_t kReleaseSize = 16;
/** Leave carries the release fields and one trailing scalar in exactly 20 bytes. */
inline constexpr std::size_t kLeaveSize = 20;
/** Connectivity failure is 66 meaningful bits, padded to 9 bytes. */
inline constexpr std::size_t kConnectivityFailureSize = 9;
/** Migration carries a biased value and a peer key in exactly 12 bytes. */
inline constexpr std::size_t kMigrationSize = 12;

/** The reason selector is two bits wide. Its schema bias is not recovered, so it stays raw. */
inline constexpr std::uint8_t kFailureReasonWidth = 2;

/**
 * One returned peer reservation.
 * The reservation context is recovered; what the two scalars mean individually is not, so they
 * keep their wire order and no meaning is assigned to either.
 */
struct ReservationRelease {
    std::uint32_t scalarA{};
    std::uint32_t scalarB{};
    /** Eight key bytes, low byte first, naming the peer inside the session's ledger. */
    std::uint64_t peerKey{};
};

/** One peer leave notice. It is the release body plus one trailing scalar. */
struct PeerLeave {
    std::uint32_t scalarA{};
    std::uint32_t scalarB{};
    std::uint64_t peerKey{};
    std::uint32_t scalarC{};
};

/** One reported failure to reach a peer. */
struct ConnectivityFailure {
    std::uint64_t peerKey{};
    /** Raw two-bit selector. The schema bias is unrecovered, so this is not a decoded reason. */
    std::uint8_t rawReason{};
};

/** One proposed host migration. Accepting one needs the group migration state machine. */
struct MigrationProposal {
    /** Biased signed scalar whose meaning is not recovered. */
    std::int32_t scalar{};
    std::uint64_t peerKey{};
};

/**
 * Parses a reservation release.
 * @param input Activity payload after the envelope.
 * @param release Cleared first, then filled.
 * @param consumedBits Receives the bits the body used, whether or not it parsed.
 * @return True when the whole fixed body was present.
 */
[[nodiscard]] bool parse_release(std::span<const std::byte> input,
                                 ReservationRelease& release,
                                 std::size_t& consumedBits) noexcept;

/**
 * Parses a peer leave notice.
 * @param input Activity payload after the envelope.
 * @param leave Cleared first, then filled.
 * @param consumedBits Receives the bits the body used, whether or not it parsed.
 * @return True when the whole fixed body was present.
 */
[[nodiscard]] bool
parse_leave(std::span<const std::byte> input, PeerLeave& leave, std::size_t& consumedBits) noexcept;

/**
 * Parses a connectivity failure report.
 * @param input Activity payload after the envelope.
 * @param failure Cleared first, then filled.
 * @param consumedBits Receives the bits the body used, whether or not it parsed.
 * @return True when the whole fixed body was present.
 */
[[nodiscard]] bool parse_connectivity_failure(std::span<const std::byte> input,
                                              ConnectivityFailure& failure,
                                              std::size_t& consumedBits) noexcept;

/**
 * Parses a speculative migration proposal.
 * @param input Activity payload after the envelope.
 * @param proposal Cleared first, then filled.
 * @param consumedBits Receives the bits the body used, whether or not it parsed.
 * @return True when the whole fixed body was present.
 */
[[nodiscard]] bool parse_migration(std::span<const std::byte> input,
                                   MigrationProposal& proposal,
                                   std::size_t& consumedBits) noexcept;

} // namespace sunrise::middleware::bap::activity_message::peer_ledger
