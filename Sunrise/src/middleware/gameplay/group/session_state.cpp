#include "session_state.h"

#include <algorithm>

#include "../../crypto/lookup3.h"

namespace sunrise::middleware::gameplay::group {

namespace {

// --- Protobuf region offsets ------------------------------------------------------------------
// These are absolute byte offsets into the replica. The peer hashes the whole struct, so every
// one of them must match its own layout exactly.

/** Membership revision, protobuf field 1. */
constexpr std::size_t kRevisionOffset = 4;
/** Host member index, field 2. */
constexpr std::size_t kHostIndexOffset = 12;
/** Host-succession candidate, field 3. */
constexpr std::size_t kSuccessionOffset = 20;
/** Member count, field 4. */
constexpr std::size_t kMemberCountOffset = 28;
/** Member-slot bitmask, field 5. */
constexpr std::size_t kMemberMaskOffset = 40;
/** Element count of the repeated member field, which the apply copies as eight bytes. */
constexpr std::size_t kMemberArrayCountOffset = 48;
/** First member entry, field 6. */
constexpr std::size_t kMemberArrayOffset = 56;
/** Bytes one member entry occupies. */
constexpr std::size_t kMemberStride = 184;

/** NetAddr length, member field 1. The value slot holds 88 bytes and the blob fills 86. */
constexpr std::size_t kMemberAddressLengthOffset = 8;
/** See kMemberAddressLengthOffset. */
constexpr std::size_t kMemberAddressOffset = 16;
/** Machine-id length, member field 2. */
constexpr std::size_t kMemberMachineLengthOffset = 112;
/** See kMemberMachineLengthOffset. */
constexpr std::size_t kMemberMachineOffset = 120;
/** Join id, member field 3. */
constexpr std::size_t kMemberJoinIdOffset = 136;
/** Player count, member field 9. */
constexpr std::size_t kMemberPlayerCountOffset = 164;
/** Element count of the repeated player-slot field, member field 10. */
constexpr std::size_t kMemberPlayerSlotCountOffset = 168;
/** See kMemberPlayerSlotCountOffset. */
constexpr std::size_t kMemberPlayerSlotOffset = 176;

// --- Peer table -------------------------------------------------------------------------------
// The only region outside the protobuf a complete snapshot writes. Indexed by member index.

/** First peer entry. */
constexpr std::size_t kPeerTableOffset = 5968;
/** Bytes one peer entry occupies. */
constexpr std::size_t kPeerStride = 288;
/** Connection state, the field the consumer's own scan reads. */
constexpr std::size_t kPeerStateOffset = 0;
/** The eight-bit delta field, stored as a word. */
constexpr std::size_t kPeerValueOffset = 4;
/** Join compatibility. */
constexpr std::size_t kPeerCompatibilityOffset = 8;
/** Join timestamp. */
constexpr std::size_t kPeerTimestampOffset = 16;

// --- Player table -----------------------------------------------------------------------------
// Mirrors the peer table. An applied row also sets two words to all ones.

/** Live player count. */
constexpr std::size_t kPlayerCountOffset = 15184;
/** Occupied player-slot mask. */
constexpr std::size_t kPlayerMaskOffset = 15188;
/** First player entry. */
constexpr std::size_t kPlayerTableOffset = 15192;
/** Bytes one player entry occupies. */
constexpr std::size_t kPlayerStride = 424;
/** Player identity. */
constexpr std::size_t kPlayerIdOffset = 4;
/** Member index that owns the player. */
constexpr std::size_t kPlayerMemberOffset = 12;
/** The owning member's own player index, which the decoder forces to zero. */
constexpr std::size_t kPlayerOwnedIndexOffset = 16;
/** Session player-add counter. */
constexpr std::size_t kPlayerSequenceOffset = 20;
/** The one-bit field, stored as a byte. */
constexpr std::size_t kPlayerFlagOffset = 24;
/** First of the two words the apply sets to all ones, before any profile group. */
constexpr std::size_t kPlayerClearedFirstOffset = 28;
/** Second of the two words the apply sets to all ones. */
constexpr std::size_t kPlayerClearedSecondOffset = 264;
/** Value both cleared words take. */
constexpr std::uint64_t kPlayerCleared = 0xFFFFFFFF;

// --- Profile group ----------------------------------------------------------------------------
// A published profile writes its kind over the first word. The complete 136-byte delta
// writes four minus-one scalar bytes over the second word.

/** The 32-bit value the profile group leads with. */
constexpr std::size_t kPlayerProfileValueOffset = 0;
/** The player kind, stored as a word over the first cleared word. */
constexpr std::size_t kPlayerProfileKindOffset = 28;
constexpr std::size_t kPlayerProfileNameOffset = 0x20;
constexpr std::size_t kPlayerProfileSubANameOffset = 0x10C;
constexpr std::size_t kPlayerProfileTailWordOffset = 0x190;
constexpr std::size_t kPlayerProfileTailFiveOffset = 0x19C;
constexpr std::size_t kPlayerProfileTailTwoOffset = 0x19D;
constexpr std::size_t kPlayerProfileTailOneOffset = 0x19E;
/** Length the consumer requires of a member NetAddr. */
constexpr std::uint64_t kAddressLength = 86;
/** Length the consumer requires of a member machine id. */
constexpr std::uint64_t kMachineIdLength = 8;
/** Player slots one member may own. */
constexpr std::uint64_t kOwnedPlayerCount = 1;

/** Starting value the three lookup3 accumulators share. A fixed literal, not length-derived. */
constexpr std::uint32_t kHashInitial = 0xDEAE2F4E;

/** Bits in one byte. */
constexpr unsigned kByteBits = 8;
/** Mask of one byte. */
constexpr std::uint64_t kByteMask = 0xFF;

/**
 * Writes one little-endian integer into the replica.
 * @param output Replica being filled.
 * @param offset Byte offset of the field.
 * @param value Value to store.
 * @param width Bytes the field occupies.
 */
void write_integer(SessionState& output,
                   std::size_t offset,
                   std::uint64_t value,
                   std::size_t width) noexcept {
    for (std::size_t index = 0; index < width; ++index) {
        output[offset + index] = static_cast<std::byte>((value >> (index * kByteBits)) & kByteMask);
    }
}

} // namespace

/** Fills a replica of the session state a peer holds after applying one complete snapshot. */
void build_session_state(const MembershipUpdate& body, SessionState& output) noexcept {
    output = {};
    const std::uint64_t count = static_cast<std::uint64_t>(body.members.size());
    // Members occupy indices 0 upward, so the mask follows from the count. The encoder derives
    // it the same way, and a mismatch changes the hash.
    const std::uint64_t mask = count == 0 ? 0 : (std::uint64_t{1} << count) - 1;

    write_integer(output, kRevisionOffset, body.revision, sizeof(std::uint32_t));
    write_integer(output, kHostIndexOffset, body.hostMemberIndex, sizeof(std::uint32_t));
    write_integer(output, kSuccessionOffset, body.successionIndex, sizeof(std::uint32_t));
    write_integer(output, kMemberCountOffset, count, sizeof(std::uint32_t));
    write_integer(output, kMemberMaskOffset, mask, sizeof(std::uint64_t));
    write_integer(output, kMemberArrayCountOffset, count, sizeof(std::uint64_t));

    for (std::size_t index = 0; index < body.members.size(); ++index) {
        const MembershipMember& member = body.members[index];
        const std::size_t entry = kMemberArrayOffset + kMemberStride * index;
        write_integer(
            output, entry + kMemberAddressLengthOffset, kAddressLength, sizeof(kAddressLength));
        for (std::size_t byte = 0; byte < member.address.size(); ++byte) {
            output[entry + kMemberAddressOffset + byte] = member.address[byte];
        }
        write_integer(
            output, entry + kMemberMachineLengthOffset, kMachineIdLength, sizeof(kMachineIdLength));
        write_integer(
            output, entry + kMemberMachineOffset, member.machineId, sizeof(std::uint64_t));
        write_integer(output, entry + kMemberJoinIdOffset, member.joinId, sizeof(std::uint64_t));
        if (member.ownsPlayerSlot) {
            write_integer(
                output, entry + kMemberPlayerCountOffset, kOwnedPlayerCount, sizeof(std::uint32_t));
            write_integer(output,
                          entry + kMemberPlayerSlotCountOffset,
                          kOwnedPlayerCount,
                          sizeof(kOwnedPlayerCount));
            write_integer(
                output, entry + kMemberPlayerSlotOffset, member.playerSlot, sizeof(std::uint32_t));
        }

        // One peer entry per member, matching what the encoder publishes. The peer clears this
        // table before applying, so a member with no connection block leaves the values zero.
        const std::size_t peer = kPeerTableOffset + kPeerStride * index;
        write_integer(output,
                      peer + kPeerStateOffset,
                      static_cast<std::uint64_t>(member.state),
                      sizeof(std::uint32_t));
        if (member.connectionPresent) {
            write_integer(
                output, peer + kPeerValueOffset, member.connectionValue, sizeof(std::uint32_t));
            write_integer(output,
                          peer + kPeerCompatibilityOffset,
                          member.joinCompatibility,
                          sizeof(std::uint32_t));
            write_integer(
                output, peer + kPeerTimestampOffset, member.joinTimestamp, sizeof(std::uint64_t));
        }
    }

    std::uint64_t playerMask = 0;
    for (const MembershipPlayer& player : body.players) {
        playerMask |= std::uint64_t{1} << player.slot;
        const std::size_t entry = kPlayerTableOffset + kPlayerStride * player.slot;
        write_integer(output, entry + kPlayerIdOffset, player.playerId, sizeof(std::uint64_t));
        write_integer(
            output, entry + kPlayerMemberOffset, player.memberIndex, sizeof(std::uint32_t));
        write_integer(output, entry + kPlayerOwnedIndexOffset, 0, sizeof(std::uint32_t));
        write_integer(
            output, entry + kPlayerSequenceOffset, player.addSequence, sizeof(std::uint32_t));
        write_integer(
            output, entry + kPlayerFlagOffset, player.flag ? 1U : 0U, sizeof(std::uint8_t));
        if (!player.hasProfile) {
            write_integer(
                output, entry + kPlayerClearedFirstOffset, kPlayerCleared, sizeof(std::uint32_t));
            write_integer(
                output, entry + kPlayerClearedSecondOffset, kPlayerCleared, sizeof(std::uint32_t));
            continue;
        }
        write_integer(
            output, entry + kPlayerProfileValueOffset, player.profileValue, sizeof(std::uint32_t));
        write_integer(
            output, entry + kPlayerProfileKindOffset, player.profileKind, sizeof(std::uint32_t));
        write_integer(
            output, entry + kPlayerClearedSecondOffset, kPlayerCleared, sizeof(std::uint32_t));
        NativePlayerProfile profile = player.nativeProfile;
        profile.soids = {true, player.accountSoid, player.characterSoid};
        NativePlayerProfileState decoded;
        build_native_player_profile_state(profile, decoded);
        std::copy(decoded.begin(),
                  decoded.end(),
                  output.begin() + static_cast<std::ptrdiff_t>(entry + kPlayerProfileNameOffset));
        // The native add initializes A scalars to -1 (177820E);
        // A's display-name copy is taken from this player's own decoded B name.
        std::copy_n(decoded.begin(),
                    kNativePlayerNameCapacity * sizeof(char16_t),
                    output.begin()
                        + static_cast<std::ptrdiff_t>(entry + kPlayerProfileSubANameOffset));
        for (std::size_t word = 0; word < profile.tailWords.size(); ++word) {
            write_integer(output,
                          entry + kPlayerProfileTailWordOffset + word * 4,
                          profile.tailWords[word],
                          4);
        }
        write_integer(output, entry + kPlayerProfileTailFiveOffset, profile.tailFlags, 1);
        write_integer(output, entry + kPlayerProfileTailTwoOffset, profile.tailKind, 1);
        write_integer(output, entry + kPlayerProfileTailOneOffset, profile.tailFlag ? 1U : 0U, 1);
        if (profile.tailFlags & 0x10) {
            write_integer(output, entry + 0x1A0, profile.tailIndex, 4);
        }
    }
    write_integer(output, kPlayerCountOffset, body.players.size(), sizeof(std::uint32_t));
    write_integer(output, kPlayerMaskOffset, playerMask, sizeof(std::uint32_t));
}

/** Computes the state hash a peer will expect for one complete snapshot. */
std::uint32_t session_state_hash(const MembershipUpdate& body) noexcept {
    // The replica is 28 KiB, too large for a stack frame on a game thread.
    static thread_local SessionState state{};
    build_session_state(body, state);
    return crypto::lookup3::hash_bytes(state, kHashInitial);
}

} // namespace sunrise::middleware::gameplay::group
