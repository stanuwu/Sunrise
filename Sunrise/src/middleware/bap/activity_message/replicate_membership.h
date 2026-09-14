#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../encoding/bit_writer.h"
#include "../../gameplay/descriptor/join_descriptor.h"
#include "activity_client_identity_parser.h"
#include "client_authoritative_data.h"

namespace sunrise::middleware::bap::activity_message::replicate_membership {

/** Membership snapshots use activity message type 12. */
inline constexpr std::uint32_t kMessageType = 12;
/** One local player and an explicit empty view mask use 30,085 meaningful bits. */
inline constexpr std::size_t kMeaningfulBitCount = 30'085;
/** The one-member snapshot has three zero padding bits. */
inline constexpr std::size_t kEncodedSize = 3'761;
/** A remote row adds its channel, process identity, view identity, and player snapshot. */
inline constexpr std::size_t kRemoteMemberBitDelta = 3'052;
/** The complete two-member body carries 33,137 meaningful bits. */
inline constexpr std::size_t kRemoteHostMeaningfulBitCount = 33'137;
/** Byte extent of the two-member body, including its seven zero padding bits. */
inline constexpr std::size_t kRemoteHostEncodedSize = 4'143;
/** One filled descriptor makes its record 1,024 bits longer and shifts every later field. */
inline constexpr std::size_t kDescriptorBitCount = gameplay::descriptor::kDescriptorSize * 8U;
/** Byte size once one record carries a descriptor. */
inline constexpr std::size_t kCitizenEncodedSize =
    kEncodedSize + gameplay::descriptor::kDescriptorSize;
/**
 * Regions one body may advertise a host for.
 * The client picks the record by region index, so a body carrying several lets it join a region
 * it is not in yet. That is what a fast travel needs and a bubble crossing does not.
 */
inline constexpr std::size_t kCitizenCapacity = 8;

/**
 * One remote-citizen advertisement placed in a single region record.
 * The record is picked by region index. The client adopts only the record whose index matches
 * its pending region.
 */
struct CitizenAdvertisement final {
    std::array<std::byte, gameplay::descriptor::kDescriptorSize> descriptor{};
    /** The ambassador's activity-host id. It is not the descriptor's own session id. */
    std::uint64_t onlineSessionId{};
    /** Region index of the record that carries it. */
    std::int32_t regionIndex{};
    /**
     * Ambassador member slot. It must differ from the joining client's own slot. An equal slot
     * picks the local-ambassador stage instead of the citizen stage.
     */
    std::uint8_t ambassadorSlot{};
    bool present{};
};

/** One remote membership row that opens a family-6 channel and replication view. */
struct RemoteViewMember final {
    /** Reflected process-session id. The peer checksum and native peer table retain this string. */
    std::array<std::int8_t, 128> processSessionId{};
    /** FNV-1a hash of processSessionId through its first NUL. */
    std::uint64_t processSessionIdHash{};
    /** Reflected player name. The view copies its first 16 bytes as the remote identity. */
    std::array<std::int8_t, 64> playerName{};
    client_identity::ClientIdentity identity{};
    std::array<std::byte, gameplay::descriptor::kNetAddrSize> address{};
    bool present{};
};

/**
 * One region leg of the local member's transition block, mirrored from the client's own report.
 * An absent leg writes its presence bit clear, which the delta codec reads as unchanged.
 */
struct RegionLeg final {
    std::int32_t sliceSetIndex{-1};
    std::uint32_t sliceSetHash{};
    std::int32_t regionIndex{-1};
    std::int8_t publicState{-1};
    std::int8_t auxState{-1};
    bool present{};
};

/** A real admitted player. Reservation alone supplies no transition or readiness state. */
struct PeerMember final {
    client_identity::ClientIdentity identity{};
    TransportReport transport{};
    RegionLeg currentLeg{};
    RegionLeg pendingLeg{};
    /** Explicit native slot; the unset value retains the original slots 2 through 31. */
    std::uint8_t slot{0xFF};
    std::uint8_t transitionToken{};
    std::uint8_t syncToken{};
    bool hasTransitionToken{};
    bool hasSyncToken{};
    std::array<std::uint16_t, 63> name{};
    std::uint8_t nameLength{};
    bool hasName{};
    bool present{};
};
inline constexpr std::size_t kPeerMemberCapacity = 30;

/** A present leg adds its five scalars and two clear presence bits. */
inline constexpr std::size_t kRegionLegBitCount = 10 + 32 + 32 + 2 + 2 + 1 + 1;
/** Ten whole bytes, so a leg never moves the body's padding. */
inline constexpr std::size_t kRegionLegByteCount = kRegionLegBitCount / 8;
static_assert(kRegionLegByteCount * 8 == kRegionLegBitCount);

/** Inputs for one local-player membership snapshot. */
struct MembershipSnapshot final {
    /** Actual local row in this activity generation. Slot one belongs to the service host. */
    std::uint8_t localSlot{};
    client_identity::ClientIdentity identity{};
    /**
     * Activity Host id every member row publishes as player-state field 4.
     * The client sends zero there in its own join request, so the host names itself. It is the
     * activity session soid, the same value the join result carries as its field 1.
     */
    std::uint64_t activityHostId{};
    /** The local member's own current and pending region legs. */
    RegionLeg currentLeg{};
    RegionLeg pendingLeg{};
    /** Optional remote host in member slot one. Absent keeps the established one-member body. */
    RemoteViewMember remoteViewMember{};
    /** Peers keep their native slots even when the recipient is not the first player. */
    std::array<PeerMember, kPeerMemberCapacity> peers{};
    client_authoritative_data::SpawnState spawn{};
    client_authoritative_data::TeleportState teleport{};
    /**
     * Host directory. One entry per region we advertise a host for, in no particular order.
     * Empty unless the gameplay channel is advertising an endpoint this run.
     */
    std::array<CitizenAdvertisement, kCitizenCapacity> citizens{};
    /** Filled entries at the front of the directory. */
    std::uint8_t citizenCount{};
    std::uint32_t revision{};
    /** Stable session epoch; changing it clears the client's peer table. */
    std::uint32_t epoch{};
    /** Transition token written into the lane of every occupied member of every region. */
    std::uint8_t transitionToken{};
    /**
     * One bit per region record, set when that record's authored bubble is public.
     * Publicity is a property of the bubble, so all 8 of its region states share one bit.
     */
    std::uint64_t regionPublicMask{};
    /**
     * True for a private activity. No bubble host is advertised, and every region record names
     * the client's own slot as ambassador. The client then hosts its bubbles as a private
     * fireteam does, and moves between them without a host change.
     */
    bool selfHosted{};
    /**
     * Region a self-hosted body publishes, or -1. Its bubble's record names it instead of the
     * bubble's state-zero region, so a state above zero has a record the client can match.
     */
    std::int32_t selfHostedRegion{-1};
};

/** The local member always occupies member slot zero. */
inline constexpr std::uint32_t kLocalMemberMask = 1U;
/** A remote view host occupies member slot one. */
inline constexpr std::uint32_t kRemoteMemberMask = 1U << 1U;

/** The default preserves the established reservation-only row layout. */
[[nodiscard]] constexpr std::uint8_t peer_slot(const MembershipSnapshot& snapshot,
                                               std::size_t index) noexcept {
    return snapshot.peers[index].slot == 0xFF ? static_cast<std::uint8_t>(index + 2)
                                              : snapshot.peers[index].slot;
}
[[nodiscard]] constexpr const PeerMember* peer_at_slot(const MembershipSnapshot& snapshot,
                                                       std::size_t slot) noexcept {
    for (std::size_t i = 0; i < snapshot.peers.size(); ++i) {
        if (snapshot.peers[i].present && peer_slot(snapshot, i) == slot) {
            return &snapshot.peers[i];
        }
    }
    return nullptr;
}
[[nodiscard]] constexpr bool has_peer_transition(const PeerMember& peer) noexcept {
    return peer.currentLeg.present || peer.pendingLeg.present || peer.hasSyncToken;
}

/** @return Mask of the member slots this body fills. */
[[nodiscard]] constexpr std::uint32_t
occupied_member_mask(const MembershipSnapshot& snapshot) noexcept {
    std::uint32_t mask = (snapshot.localSlot < 32 ? 1U << snapshot.localSlot : 0U)
                         | (snapshot.remoteViewMember.present ? kRemoteMemberMask : 0U);
    for (std::size_t i = 0; i < snapshot.peers.size(); ++i) {
        if (snapshot.peers[i].present && peer_slot(snapshot, i) < 32) {
            mask |= 1U << peer_slot(snapshot, i);
        }
    }
    return mask;
}

/** The native view updater skips the own member and activates the advertised remote member. */
[[nodiscard]] constexpr std::uint32_t
active_view_mask(const MembershipSnapshot& snapshot) noexcept {
    std::uint32_t mask = snapshot.remoteViewMember.present ? kRemoteMemberMask : 0U;
    for (std::size_t i = 0; i < snapshot.peers.size(); ++i) {
        const auto& peer = snapshot.peers[i];
        if (peer.present && peer.transport.hasFlags && (peer.transport.flags & 0x10U)
            && peer.transport.hasAddress && peer_slot(snapshot, i) < 32) {
            mask |= 1U << peer_slot(snapshot, i);
        }
    }
    return mask;
}

[[nodiscard]] constexpr std::size_t
peer_member_bit_count(const MembershipSnapshot& snapshot) noexcept {
    std::size_t bits{};
    for (const auto& peer : snapshot.peers) {
        if (!peer.present) {
            continue;
        }
        bits += 745 + (has_peer_transition(peer) ? 5 : 0) + (peer.hasSyncToken ? 8 : 0)
                + (peer.currentLeg.present ? kRegionLegBitCount : 0)
                + (peer.pendingLeg.present ? kRegionLegBitCount : 0)
                + (peer.transport.hasFlags ? 6 : 0) + (peer.transport.hasAddress ? 688 : 0)
                + (peer.transport.hasAlternate ? 688 : 0)
                + (peer.hasName ? 32 * (static_cast<std::size_t>(peer.nameLength) + 1) : 0);
    }
    return bits;
}

/** @return Bits the local member's present region legs add. */
[[nodiscard]] constexpr std::size_t
region_leg_bit_count(const MembershipSnapshot& snapshot) noexcept {
    return (snapshot.currentLeg.present ? kRegionLegBitCount : 0)
           + (snapshot.pendingLeg.present ? kRegionLegBitCount : 0);
}

/** @return Encoded byte size for one snapshot, which grows with each advertised region. */
[[nodiscard]] constexpr std::size_t encoded_size(const MembershipSnapshot& snapshot) noexcept {
    return (kMeaningfulBitCount + (snapshot.remoteViewMember.present ? kRemoteMemberBitDelta : 0)
            + snapshot.citizenCount * kDescriptorBitCount + region_leg_bit_count(snapshot)
            + peer_member_bit_count(snapshot) + 7)
           / 8;
}

/** @return Meaningful bits before byte padding for this exact member/directory shape. */
[[nodiscard]] constexpr std::size_t
meaningful_bit_count(const MembershipSnapshot& snapshot) noexcept {
    return kMeaningfulBitCount + (snapshot.remoteViewMember.present ? kRemoteMemberBitDelta : 0)
           + snapshot.citizenCount * kDescriptorBitCount + region_leg_bit_count(snapshot)
           + peer_member_bit_count(snapshot);
}

/** The host table has one fixed record per bubble, and the client reads at most 64 of them. */
inline constexpr std::size_t kRegionRecordCount = 64;
/** A bubble owns 8 slice-set states, so its regions run `8 * bubble` to `8 * bubble + 7`. */
inline constexpr std::int32_t kRegionStateCount = 8;
/** First region index no record can name. */
inline constexpr std::int32_t kRegionIndexBound =
    static_cast<std::int32_t>(kRegionRecordCount) * kRegionStateCount;

/**
 * Finds the directory entry one bubble's region record carries.
 * A record names one region, so a bubble can advertise only one of its 8 states at a time.
 * @param snapshot Snapshot holding the directory.
 * @param bubble Bubble ordinal of the record being written.
 * @return The entry in that bubble, or null when the directory names none.
 */
[[nodiscard]] constexpr const CitizenAdvertisement*
advertisement_in_bubble(const MembershipSnapshot& snapshot, std::size_t bubble) noexcept {
    for (std::size_t entry = 0; entry < snapshot.citizenCount; ++entry) {
        const CitizenAdvertisement& candidate = snapshot.citizens[entry];
        if (candidate.present && candidate.regionIndex >= 0
            && static_cast<std::size_t>(candidate.regionIndex / kRegionStateCount) == bubble) {
            return &candidate;
        }
    }
    return nullptr;
}

/**
 * Encodes one full-player membership snapshot. No allocation.
 * @param snapshot Checked identity, revision, transition, and host-echo values.
 * @param output Caller storage, left unchanged when validation fails or it is too small.
 * @param written Receives the encoded byte count on success or zero on failure.
 * @return True when the host-present body was encoded.
 */
[[nodiscard]] bool encode_replicate_membership(const MembershipSnapshot& snapshot,
                                               std::span<std::byte> output,
                                               std::size_t& written) noexcept;

/** The local member begins after root, revision, and epoch fields. */
inline constexpr std::size_t kMemberStartBit = 65;
/** The local identity alone shifts the region block to bit 920. */
inline constexpr std::size_t kRegionBlockStartBit = 920;
/** The host-present region block ends before top-level field four. */
inline constexpr std::size_t kRegionBlockEndBit = 29'984;

/** @return Bit at which the region block ends for one snapshot. */
[[nodiscard]] constexpr std::size_t
region_block_end_bit(const MembershipSnapshot& snapshot) noexcept {
    return kRegionBlockEndBit + (snapshot.remoteViewMember.present ? kRemoteMemberBitDelta : 0)
           + snapshot.citizenCount * kDescriptorBitCount + region_leg_bit_count(snapshot)
           + peer_member_bit_count(snapshot);
}

/** @return First bit of region zero after the exact member-table shape. */
[[nodiscard]] constexpr std::size_t
region_block_start_bit(const MembershipSnapshot& snapshot) noexcept {
    return kRegionBlockStartBit + (snapshot.remoteViewMember.present ? kRemoteMemberBitDelta : 0)
           + region_leg_bit_count(snapshot) + peer_member_bit_count(snapshot);
}

/** @return True when every snapshot field fits the fixed wire field that carries it. */
[[nodiscard]] bool valid(const MembershipSnapshot& snapshot) noexcept;

/**
 * Writes the local member, an optional remote-view member, and every remaining absent slot.
 * @param writer Fixed-buffer writer positioned at bit 65.
 * @param snapshot Exact local identity and optional remote-view row.
 * @return True when the writer reaches the region-block presence bit.
 */
[[nodiscard]] bool write_member_table(encoding::bits::Writer& writer,
                                      const MembershipSnapshot& snapshot) noexcept;

/**
 * Writes the 64 region records and the host-present tail.
 * @param writer Fixed-buffer writer positioned at the region-block start bit.
 * @param snapshot Transition token and reflected host state.
 * @return True when the writer reaches top-level field four.
 */
[[nodiscard]] bool write_region_block(encoding::bits::Writer& writer,
                                      const MembershipSnapshot& snapshot) noexcept;

} // namespace sunrise::middleware::bap::activity_message::replicate_membership
