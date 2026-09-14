#include <algorithm>
#include <bit>

#include "../../encoding/bit_raw.h"
#include "replicate_membership.h"

namespace sunrise::middleware::bap::activity_message::replicate_membership {
namespace {

/** 8 elements, low byte first, encode a member or host key. */
constexpr std::size_t kMemberKeyByteCount = 8;
/** The membership table has 32 fixed member slots. */
constexpr std::size_t kMemberCount = 32;
/** Each absent member adds 3 clear presence bits. */
constexpr std::uint8_t kAbsentMemberBitCount = 3;
/**
 * Member field 1 is the member's slice-set index, 10 bits at bias 1. A value at or below 0x1FF
 * makes the client secure the member's channel through its direct security manager. A -1 picks
 * the owner-backed manager, which waits on a session a bubble host never gets.
 */
constexpr std::uint32_t kField1Bias = 1;
/** Member field 2 uses the signed 32-bit midpoint as its bias. */
constexpr std::uint32_t kField2Bias = 0x80000000U;
/** The member transition block's synchronisation byte is a plain 8-bit field. */
constexpr std::uint8_t kSyncTokenBitWidth = 8;
/** A leg's fields 3 and 4 are 2-bit signed values at bias 1. */
constexpr std::uint8_t kLegStateWidth = 2;
constexpr std::int32_t kLegStateBias = 1;
/** A present local member carries a zero logical leave reason at bias 1. */
constexpr std::uint8_t kLeaveReasonWire = 1;
/** The nested identity block has presence bits on fields 0 through 14. */
constexpr std::size_t kIdentityPresenceFieldCount = 15;
/** The player blob includes the native partition field and three zero tail pad bits. */
constexpr std::uint16_t kPlayerBlobByteCount = 19;
/** Native A.P2 uses a six-bit value at bias one; free-roam partition zero is wire one. */
constexpr std::uint8_t kPlayerPartitionWire = 1;
/** The remote member's player-state field zero carries the native-view gate. */
constexpr std::uint8_t kRemoteViewGate = 0x10;
/** Player-state field zero is a six-bit scalar. */
constexpr std::uint8_t kRemoteViewGateWidth = 6;
/** Reflected character arrays add 128 to each signed source byte. */
constexpr std::uint8_t kStringBias = 128;
/** Standard FNV-1a constants verify the process-session id pair. */
constexpr std::uint64_t kFnv1aBasis = 0xCBF29CE484222325ULL;
constexpr std::uint64_t kFnv1aPrime = 0x100000001B3ULL;

/** @return True when a fixed reflected string contains its terminator. */
template <std::size_t Size>
[[nodiscard]] bool terminated(const std::array<std::int8_t, Size>& value) noexcept {
    return std::find(value.begin(), value.end(), 0) != value.end();
}

/** @return FNV-1a through the first NUL of one reflected string. */
template <std::size_t Size>
[[nodiscard]] std::uint64_t hash_string(const std::array<std::int8_t, Size>& value) noexcept {
    std::uint64_t hash = kFnv1aBasis;
    for (const std::int8_t character : value) {
        if (character == 0) {
            break;
        }
        hash ^= static_cast<std::uint8_t>(character);
        hash *= kFnv1aPrime;
    }
    return hash;
}

/** Writes one complete signed reflected character region. */
template <std::size_t Size>
[[nodiscard]] bool write_string(encoding::bits::Writer& writer,
                                const std::array<std::int8_t, Size>& value) noexcept {
    for (const std::int8_t character : value) {
        const auto encoded =
            static_cast<std::uint8_t>(static_cast<std::int32_t>(character) + kStringBias);
        if (!writer.write(encoded, 8)) {
            return false;
        }
    }
    return true;
}

/** Writes one 8-byte key, low byte first. */
[[nodiscard]] bool write_member_key(encoding::bits::Writer& writer, std::uint64_t key) noexcept {
    for (std::size_t index = 0; index < kMemberKeyByteCount; ++index) {
        if (!writer.write((key >> (index * 8U)) & 0xFFU, 8)) {
            return false;
        }
    }
    return true;
}

/** Writes the player blob, including the partition consumed by native participation sensing. */
[[nodiscard]] bool write_player_blob(encoding::bits::Writer& writer,
                                     const client_identity::ClientIdentity& identity,
                                     const PeerMember* peer) noexcept {
    const bool named = peer != nullptr && peer->hasName;
    const auto name = [&]() noexcept {
        if (!writer.write(named ? 1U : 0U, 1)) {
            return false;
        }
        if (!named) {
            return true;
        }
        for (std::size_t i = 0; i < peer->nameLength; ++i) {
            if (!writer.write(peer->name[i], 16)) {
                return false;
            }
        }
        return writer.write(0, 16);
    };
    return writer.write(1, 3) && writer.write(0, 1) && name() && writer.write(1, 1)
           && writer.write(kPlayerPartitionWire, 6) && writer.write(0, 2) && name()
           && writer.write(0, 5) && writer.write(1, 1) && writer.write(identity.accountSoid, 64)
           && writer.write(identity.field5, 64) && writer.write(0, 3);
}

/**
 * Writes the present fields of the nested player identity block.
 * Field 3 carries the account SOID and field 5 the character SOID; the client builds its player
 * row from that pair and creates no player without it. Field 4 names the Activity Host.
 * @param writer Fixed-buffer writer sitting at identity field zero.
 * @param identity Values mirrored from the client identity update.
 * @param remote Optional remote-view row. Null writes the local member's shorter field set.
 * @param activityHostId Host id for field 4, which the client itself always sends as zero.
 * @return True when fields 3, 4, 5 and 14 and all presence bits fit.
 */
[[nodiscard]] bool write_player_identity(encoding::bits::Writer& writer,
                                         const client_identity::ClientIdentity& identity,
                                         const RemoteViewMember* remote,
                                         std::uint64_t activityHostId,
                                         const PeerMember* peer) noexcept {
    for (std::size_t field = 0; field < kIdentityPresenceFieldCount; ++field) {
        const bool present =
            field == 3 || field == 4 || field == 5 || field == 14
            || (remote != nullptr
                && (field == 0 || field == 1 || field == 2 || field == 11 || field == 13))
            || (peer != nullptr
                && ((field == 0 && peer->transport.hasFlags)
                    || (field == 10 && peer->transport.hasAlternate)
                    || (field == 11 && peer->transport.hasAddress)));
        if (!writer.write(present ? 1U : 0U, 1)) {
            return false;
        }
        if (field == 0 && remote != nullptr
            && !writer.write(kRemoteViewGate, kRemoteViewGateWidth)) {
            return false;
        }
        if (field == 0 && peer != nullptr && peer->transport.hasFlags
            && !writer.write(peer->transport.flags, 6)) {
            return false;
        }
        if (field == 10 && peer != nullptr && peer->transport.hasAlternate
            && !encoding::bits::write_raw(writer, peer->transport.alternate)) {
            return false;
        }
        if (field == 11 && peer != nullptr && peer->transport.hasAddress
            && !encoding::bits::write_raw(writer, peer->transport.address)) {
            return false;
        }
        if (field == 1 && remote != nullptr && !write_string(writer, remote->processSessionId)) {
            return false;
        }
        if (field == 2 && remote != nullptr && !writer.write(remote->processSessionIdHash, 64)) {
            return false;
        }
        if (field == 3 && !writer.write(identity.accountSoid, 64)) {
            return false;
        }
        if (field == 4 && !writer.write(activityHostId, 64)) {
            return false;
        }
        if (field == 5 && !writer.write(identity.field5, 64)) {
            return false;
        }
        if (field == 11 && remote != nullptr
            && !encoding::bits::write_raw(writer, remote->address)) {
            return false;
        }
        if (field == 13 && remote != nullptr && !write_string(writer, remote->playerName)) {
            return false;
        }
        if (field == 14
            && (!writer.write(
                    kPlayerBlobByteCount
                        + (peer != nullptr && peer->hasName ? 4U * (peer->nameLength + 1U) : 0U),
                    14)
                || !write_player_blob(writer, identity, peer))) {
            return false;
        }
    }
    return writer.write(0, 1);
}

/**
 * Writes one region leg of the transition block, starting with its presence bit.
 * When present it also writes the five scalars the client reported, then clear presence bits
 * for the aux token and the session block.
 * @param writer Fixed-buffer writer sitting at the leg's presence bit.
 * @param leg Mirrored client report, or an absent leg.
 * @return True when the bits fit.
 */
[[nodiscard]] bool write_region_leg(encoding::bits::Writer& writer, const RegionLeg& leg) noexcept {
    if (!leg.present) {
        return writer.write(0, 1);
    }
    const std::uint32_t sliceSetWire =
        std::bit_cast<std::uint32_t>(leg.sliceSetIndex) + kField1Bias;
    const std::uint32_t regionWire = std::bit_cast<std::uint32_t>(leg.regionIndex) + kField2Bias;
    const auto publicWire = static_cast<std::uint32_t>(leg.publicState + kLegStateBias);
    const auto auxWire = static_cast<std::uint32_t>(leg.auxState + kLegStateBias);
    return writer.write(1, 1) && writer.write(sliceSetWire, 10)
           && writer.write(leg.sliceSetHash, 32) && writer.write(regionWire, 32)
           && writer.write(publicWire, kLegStateWidth) && writer.write(auxWire, kLegStateWidth)
           && writer.write(0, 1) && writer.write(0, 1);
}

/**
 * Writes the member's transition block: the two region legs and the synchronisation byte.
 * Every slice-set transition blocks, with no timeout, until member-record `+10889` equals the
 * transition manager's token at `0x7FF742AE2250`.
 * @param writer Fixed-buffer writer sitting at the block's presence bit.
 * @param currentLeg The member's current region leg, present only for the local member.
 * @param pendingLeg The member's pending region leg, present only for the local member.
 * @param syncToken Token the host named in the teleport block.
 * @return True when the presence bits, the legs and the byte fit.
 */
[[nodiscard]] bool write_member_transition(encoding::bits::Writer& writer,
                                           const RegionLeg& currentLeg,
                                           const RegionLeg& pendingLeg,
                                           std::uint8_t syncToken) noexcept {
    return writer.write(1, 1) && write_region_leg(writer, currentLeg)
           && write_region_leg(writer, pendingLeg) && writer.write(0, 1) && writer.write(1, 1)
           && writer.write(syncToken, kSyncTokenBitWidth) && writer.write(0, 1);
}

/** A human peer's sparse transition fields come only from that peer's native reports. */
[[nodiscard]] bool write_peer_transition(encoding::bits::Writer& writer,
                                         const PeerMember& peer) noexcept {
    if (!has_peer_transition(peer)) {
        return writer.write(0, 1);
    }
    return writer.write(1, 1) && write_region_leg(writer, peer.currentLeg)
           && write_region_leg(writer, peer.pendingLeg) && writer.write(0, 1)
           && writer.write(peer.hasSyncToken ? 1 : 0, 1)
           && (!peer.hasSyncToken || writer.write(peer.syncToken, kSyncTokenBitWidth))
           && writer.write(0, 1);
}

/** Writes one complete occupied member row. */
[[nodiscard]] bool write_member(encoding::bits::Writer& writer,
                                const client_identity::ClientIdentity& identity,
                                const RemoteViewMember* remote,
                                const RegionLeg& currentLeg,
                                const RegionLeg& pendingLeg,
                                std::uint8_t syncToken,
                                std::uint64_t activityHostId,
                                const PeerMember* peer = nullptr) noexcept {
    const std::uint32_t field1Wire = std::bit_cast<std::uint32_t>(identity.field1) + kField1Bias;
    const std::uint32_t field2Wire = std::bit_cast<std::uint32_t>(identity.field2) + kField2Bias;
    return writer.write(1, 1) && write_member_key(writer, identity.memberKey)
           && writer.write(field1Wire, 10) && writer.write(field2Wire, 32)
           && writer.write(identity.field3, 64) && writer.write(identity.accountSoid, 64)
           && writer.write(identity.field5, 64) && writer.write(identity.field6, 64)
           && writer.write(1, 1) && writer.write(1, 1)
           && write_player_identity(writer, identity, remote, activityHostId, peer)
           && (peer != nullptr ? write_peer_transition(writer, *peer)
                               : write_member_transition(writer, currentLeg, pendingLeg, syncToken))
           && writer.write(0, 1) && writer.write(0, 1) && writer.write(1, 1)
           && writer.write(kLeaveReasonWire, 5);
}

/** @return True when a leg's scalars fit their wire fields. */
[[nodiscard]] bool valid_leg(const RegionLeg& leg) noexcept {
    namespace authoritative = client_authoritative_data;
    return !leg.present
           || (leg.sliceSetIndex >= authoritative::kAbsentLegSliceSetIndex
               && leg.sliceSetIndex <= authoritative::kMaximumSliceSetIndex
               && leg.publicState >= authoritative::kMinimumLegState
               && leg.publicState <= authoritative::kMaximumLegState
               && leg.auxState >= authoritative::kMinimumLegState
               && leg.auxState <= authoritative::kMaximumLegState);
}

/**
 * Reports whether every advertised region gets a record of its own.
 * The encoded size counts one descriptor per citizen, and each bubble has one record. So a
 * region outside the table, or a second citizen in one bubble, writes fewer bits than promised.
 * @param snapshot Snapshot holding the directory.
 * @return True when the directory is placeable.
 */
[[nodiscard]] bool placeable_directory(const MembershipSnapshot& snapshot) noexcept {
    for (std::size_t entry = 0; entry < snapshot.citizenCount; ++entry) {
        const CitizenAdvertisement& candidate = snapshot.citizens[entry];
        if (!candidate.present || candidate.regionIndex < 0
            || candidate.regionIndex >= kRegionIndexBound
            || advertisement_in_bubble(
                   snapshot, static_cast<std::size_t>(candidate.regionIndex / kRegionStateCount))
                   != &candidate) {
            return false;
        }
    }
    return true;
}

/** @return True when the optional remote row can open a direct family-6 channel. */
[[nodiscard]] bool valid_remote(const RemoteViewMember& remote) noexcept {
    if (!remote.present) {
        return true;
    }
    const bool hasAddress = std::find_if(remote.address.begin(),
                                         remote.address.end(),
                                         [](const std::byte value) { return value != std::byte{}; })
                            != remote.address.end();
    return remote.identity.memberKey != 0 && remote.identity.field1 >= 0
           && remote.identity.field1 <= 0x1FF && remote.identity.field3 != 0
           && remote.identity.accountSoid != 0 && remote.identity.field5 != 0
           && remote.identity.field6 != 0 && hasAddress
           && std::to_integer<std::uint8_t>(remote.address.back()) <= 5
           && terminated(remote.processSessionId) && terminated(remote.playerName)
           && remote.processSessionIdHash == hash_string(remote.processSessionId);
}

} // namespace

/** Checks the fields that could otherwise encode outside their own wire width. */
bool valid(const MembershipSnapshot& snapshot) noexcept {
    namespace authoritative = client_authoritative_data;
    if (snapshot.localSlot >= 32 || snapshot.localSlot == 1) {
        return false;
    }
    for (std::size_t i = 0; i < snapshot.peers.size(); ++i) {
        const auto& peer = snapshot.peers[i];
        if (!peer.present) {
            continue;
        }
        const auto slot = peer_slot(snapshot, i);
        if (slot >= 32 || slot == 1 || slot == snapshot.localSlot || !valid_leg(peer.currentLeg)
            || !valid_leg(peer.pendingLeg)) {
            return false;
        }
        if (!peer.identity.memberKey || !peer.identity.accountSoid || !peer.identity.field5
            || peer.identity.field1 < -1 || peer.identity.field1 > 1022
            || peer.nameLength > peer.name.size()
            || (peer.transport.hasFlags && peer.transport.flags > 63)
            || peer.identity.memberKey == snapshot.identity.memberKey
            || peer.identity.accountSoid == snapshot.identity.accountSoid
            || (snapshot.remoteViewMember.present
                && peer.identity.memberKey == snapshot.remoteViewMember.identity.memberKey)) {
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (snapshot.peers[j].present
                && (peer.identity.memberKey == snapshot.peers[j].identity.memberKey
                    || peer.identity.accountSoid == snapshot.peers[j].identity.accountSoid
                    || slot == peer_slot(snapshot, j))) {
                return false;
            }
        }
        for (std::size_t j = 0; peer.hasName && j < peer.nameLength; ++j) {
            if (peer.name[j] == 0) {
                return false;
            }
        }
    }
    return snapshot.teleport.sliceSetIndex >= authoritative::kAbsentSliceSetIndex
           && snapshot.teleport.sliceSetIndex <= authoritative::kMaximumSliceSetIndex
           && snapshot.selfHostedRegion < kRegionIndexBound
           && snapshot.citizenCount <= kCitizenCapacity && placeable_directory(snapshot)
           && valid_remote(snapshot.remoteViewMember) && valid_leg(snapshot.currentLeg)
           && valid_leg(snapshot.pendingLeg);
}

/** Writes the local member, optional remote-view member, and the remaining absent slots. */
bool write_member_table(encoding::bits::Writer& writer,
                        const MembershipSnapshot& snapshot) noexcept {
    const RegionLeg absentLeg{};
    bool encoded = writer.bit_count() == kMemberStartBit;
    for (std::size_t slot = 0; encoded && slot < kMemberCount; ++slot) {
        if (slot == snapshot.localSlot) {
            encoded = write_member(writer,
                                   snapshot.identity,
                                   nullptr,
                                   snapshot.currentLeg,
                                   snapshot.pendingLeg,
                                   snapshot.teleport.token,
                                   snapshot.activityHostId);
        } else if (slot == 1 && snapshot.remoteViewMember.present) {
            encoded = write_member(writer,
                                   snapshot.remoteViewMember.identity,
                                   &snapshot.remoteViewMember,
                                   absentLeg,
                                   absentLeg,
                                   snapshot.teleport.token,
                                   snapshot.activityHostId);
        } else if (const auto* peer = peer_at_slot(snapshot, slot)) {
            encoded = write_member(writer,
                                   peer->identity,
                                   nullptr,
                                   absentLeg,
                                   absentLeg,
                                   0,
                                   snapshot.activityHostId,
                                   peer);
        } else {
            encoded = writer.write(0, kAbsentMemberBitCount);
        }
    }
    return encoded && writer.bit_count() + 1 == region_block_start_bit(snapshot);
}

} // namespace sunrise::middleware::bap::activity_message::replicate_membership
