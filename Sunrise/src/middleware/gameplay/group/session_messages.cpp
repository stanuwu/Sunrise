#include "session_messages.h"

#include <array>

#include "../../encoding/bit_raw.h"
#include "../../protobuf/codec.h"
#include "player_block.h"
#include "session_state.h"

namespace sunrise::middleware::gameplay::group {

namespace {

namespace bits = encoding::bits;

/** The protocol version is a 16-bit value field. */
constexpr std::uint8_t kProtocolWidth = 16;
/** The join sequence is a 32-bit value field. */
constexpr std::uint8_t kSequenceWidth = 32;
/** Single-bit flags. */
constexpr std::uint8_t kFlagWidth = 1;
/** The boot kind is three bits. */
constexpr std::uint8_t kBootKindWidth = 3;
/** The boot reason is five bits. */
constexpr std::uint8_t kBootReasonWidth = 5;
/** Time samples are 64-bit value fields. */
constexpr std::uint8_t kSampleWidth = 64;

// --- Membership update, message id 30 -------------------------------------------------------

/** Protobuf field numbers of the membership root. */
constexpr std::uint32_t kRootRevision = 1;
/** See kRootRevision. */
constexpr std::uint32_t kRootHostIndex = 2;
/** See kRootRevision. */
constexpr std::uint32_t kRootSuccession = 3;
/** See kRootRevision. */
constexpr std::uint32_t kRootMemberCount = 4;
/** See kRootRevision. */
constexpr std::uint32_t kRootMemberMask = 5;
/** See kRootRevision. */
constexpr std::uint32_t kRootMember = 6;

/** Protobuf field numbers of one member. */
constexpr std::uint32_t kMemberAddress = 1;
/** See kMemberAddress. */
constexpr std::uint32_t kMemberMachineId = 2;
/** See kMemberAddress. */
constexpr std::uint32_t kMemberJoinId = 3;
/** See kMemberAddress. */
constexpr std::uint32_t kMemberPartyId = 8;
/** See kMemberAddress. */
constexpr std::uint32_t kMemberPlayerCount = 9;
/** See kMemberAddress. */
constexpr std::uint32_t kMemberPlayerSlot = 10;
/** See kMemberAddress. */
constexpr std::uint32_t kMemberHostLeavePending = 11;
/** See kMemberAddress. */
constexpr std::uint32_t kMemberHostLeaveDone = 12;

/** Party id. Zero takes the receiver's empty-id path, so no member is grouped with another. */
constexpr std::uint64_t kMemberPartyIdEmpty = 0;
/** Players a member with a slot owns. The codec publishes at most one slot per member. */
constexpr std::uint64_t kOwnedPlayerCount = 1;
/** No host leave is in progress, so both leave flags stay clear. */
constexpr std::uint64_t kMemberHostLeaveClear = 0;

/** The protobuf body is length-prefixed with thirteen bits. */
constexpr std::uint8_t kProtobufLengthWidth = 13;
/** Both revision words are 32 bits. */
constexpr std::uint8_t kRevisionWidth = 32;
/** Both delta counts are six bits. */
constexpr std::uint8_t kDeltaCountWidth = 6;
/** A peer-delta index is six bits. */
constexpr std::uint8_t kDeltaIndexWidth = 6;
/** A member state is four bits. */
constexpr std::uint8_t kMemberStateWidth = 4;
/** The third connection value. */
constexpr std::uint8_t kConnectionValueWidth = 8;
/** The join compatibility word, and the trailing session-state hash. */
constexpr std::uint8_t kWordWidth = 32;
/** The join timestamp. It is a value field, so it goes out most significant byte first. */
constexpr std::uint8_t kJoinTimestampWidth = 64;

/** A clear flag ahead of the revision pair publishes it. Its absence decodes as -1. */
constexpr std::uint64_t kRevisionPairPresent = 0;
/** Base revision 0 selects a complete snapshot. Any other value is a delta against it. */
constexpr std::uint32_t kCompleteSnapshotBase = 0;
/** The word after the base revision is never read by the consumer. */
constexpr std::uint32_t kUnreadWord = 0;
/** A set mode bit means a whole peer-delta entry follows the index. */
constexpr std::uint64_t kDeltaEntryFull = 1;
/** A player-delta index is five bits, one narrower than a peer-delta index. */
constexpr std::uint8_t kPlayerIndexWidth = 5;
/** The member index a player row names. */
constexpr std::uint8_t kPlayerMemberWidth = 6;
/** The member's own player index. The decoder refuses any value but zero, so a member publishes
 *  at most one player this way. */
constexpr std::uint8_t kPlayerOwnedIndexWidth = 1;
/** The session player-add counter, which the consumer keeps modulo 2^20. */
constexpr std::uint8_t kPlayerSequenceWidth = 20;
/** Value the decoder requires of the member's own player index. */
constexpr std::uint64_t kPlayerOwnedIndexZero = 0;
/** Bias the player kind rides on the wire. The decoder subtracts it. */
constexpr std::uint32_t kPlayerKindBias = 1;
/** Largest player kind the message codec accepts. */
constexpr std::uint8_t kPlayerKindMaximum = 3;
/** This host publishes no 264-byte identity block and neither trailing delta-entry flag. */
constexpr std::uint64_t kEntryFieldAbsent = 0;
/** The four tail groups are all omitted, which leaves the consumer's own values alone. */
constexpr std::uint64_t kTailGroupAbsent = 0;
/** Tail groups omitted, one presence bit each. The encoder writes four, not five. */
constexpr std::size_t kTailGroupCount = 4;
/** Largest protobuf body the message codec accepts. */
constexpr std::size_t kProtobufCapacity = 5972;
/** One encoded member submessage cannot exceed this. */
constexpr std::size_t kMemberBytes = 128;
/** Machine identities are published as eight bytes in memory order. */
constexpr std::size_t kMachineIdBytes = 8;
/** Bits in one byte. */
constexpr unsigned kByteBits = 8;
/** Mask of one byte. */
constexpr std::uint64_t kByteMask = 0xFF;

/**
 * Appends one member as a length-delimited submessage.
 * @param writer Open protobuf writer for the root message.
 * @param member Member to publish.
 * @return True when the whole submessage fit.
 */
[[nodiscard]] bool write_member(protobuf::Writer& writer, const MembershipMember& member) noexcept {
    std::array<std::byte, kMachineIdBytes> machineId{};
    for (std::size_t index = 0; index < machineId.size(); ++index) {
        machineId[index] =
            static_cast<std::byte>((member.machineId >> (index * kByteBits)) & kByteMask);
    }

    std::array<std::byte, kMemberBytes> storage{};
    protobuf::Writer body(storage);
    if (!body.write_length_delimited(kMemberAddress, member.address)
        || !body.write_length_delimited(kMemberMachineId, machineId)
        || !body.write_varint(kMemberJoinId, member.joinId)
        || !body.write_varint(kMemberPartyId, kMemberPartyIdEmpty)) {
        return false;
    }
    // The player count feeds the peer's party-size check, so a member with a slot counts it.
    if (member.ownsPlayerSlot
        && (!body.write_varint(kMemberPlayerCount, kOwnedPlayerCount)
            || !body.write_varint(kMemberPlayerSlot, member.playerSlot))) {
        return false;
    }
    if (!body.write_varint(kMemberHostLeavePending, kMemberHostLeaveClear)
        || !body.write_varint(kMemberHostLeaveDone, kMemberHostLeaveClear)) {
        return false;
    }
    return writer.write_length_delimited(kRootMember, {storage.data(), body.size()});
}

/**
 * Encodes the membership protobuf body.
 * @param body Snapshot to publish.
 * @param storage Caller-owned protobuf storage.
 * @param size Receives the encoded byte count.
 * @return True when every field fit.
 */
[[nodiscard]] bool write_membership_protobuf(const MembershipUpdate& body,
                                             std::span<std::byte> storage,
                                             std::size_t& size) noexcept {
    const std::uint64_t count = static_cast<std::uint64_t>(body.members.size());
    // Members occupy indices 0 upward, so the occupied-slot mask follows from the count.
    const std::uint64_t mask = (std::uint64_t{1} << count) - 1;
    protobuf::Writer writer(storage);
    if (!writer.write_varint(kRootRevision, body.revision)
        || !writer.write_varint(kRootHostIndex, body.hostMemberIndex)
        || !writer.write_varint(kRootSuccession, body.successionIndex)
        || !writer.write_varint(kRootMemberCount, count)
        || !writer.write_varint(kRootMemberMask, mask)) {
        return false;
    }
    for (const MembershipMember& member : body.members) {
        if (!write_member(writer, member)) {
            return false;
        }
    }
    size = writer.size();
    return true;
}

/**
 * Writes one peer-delta entry.
 * @param writer Open writer.
 * @param index Member index the entry names.
 * @param member Member whose state the entry publishes.
 * @return True when every field fit.
 */
[[nodiscard]] bool
write_peer_delta(bits::Writer& writer, std::size_t index, const MembershipMember& member) noexcept {
    if (!writer.write(index, kDeltaIndexWidth) || !writer.write(kDeltaEntryFull, kFlagWidth)
        || !writer.write(static_cast<std::uint64_t>(member.state), kMemberStateWidth)
        || !writer.write(member.connectionPresent ? 1U : 0U, kFlagWidth)) {
        return false;
    }
    if (member.connectionPresent
        && (!writer.write(member.joinCompatibility, kWordWidth)
            || !writer.write(member.joinTimestamp, kJoinTimestampWidth)
            || !writer.write(member.connectionValue, kConnectionValueWidth))) {
        return false;
    }
    return writer.write(kEntryFieldAbsent, kFlagWidth)
           && writer.write(kEntryFieldAbsent, kFlagWidth)
           && writer.write(kEntryFieldAbsent, kFlagWidth);
}

// Widths the profile group declares on the wire: its leading 32-bit value, the 3-bit player
// kind, one UTF-16 name unit, and sub-block A's remaining fields. The native record names none
// of those last three, so they are numbered here by the order the codec reads them.
constexpr std::uint8_t kProfileScalarWidth = 32;
constexpr std::uint8_t kProfileSmallWidth = 3;
constexpr std::uint8_t kProfileNameUnitWidth = 16;
constexpr std::uint8_t kProfileSubA23Width = 6;
constexpr std::uint8_t kProfileSubA4FirstWidth = 2;
constexpr std::uint8_t kProfileSubA4SecondWidth = 6;

/**
 * Writes one name payload: its code units in the clear, then the terminator.
 * The wire carries plain units; only the native record's own copy is obfuscated on the way
 * in, and restored the same way coming back out.
 */
[[nodiscard]] bool write_profile_name(bits::Writer& writer,
                                      const NativePlayerProfile& profile) noexcept {
    for (std::size_t unit = 0; unit < profile.nameLength; ++unit) {
        if (!writer.write(static_cast<std::uint64_t>(profile.name[unit]), kProfileNameUnitWidth)) {
            return false;
        }
    }
    // The native decoder consumes a terminator unless all 64 code units are occupied.
    return profile.nameLength == profile.name.size() || writer.write(0, kProfileNameUnitWidth);
}

/**
 * Writes the complete profile block of one player row -- every field the consumer copies.
 * `complete_native_player_profile` gates the call: every optional B field, the name, the
 * identity and the tail must already be present. Sub-block A follows B unconditionally, its
 * presence flags forced true and its fields zero except the name, which duplicates B's.
 */
[[nodiscard]] bool write_player_profile(bits::Writer& writer,
                                        const NativePlayerProfile& profile,
                                        std::uint32_t value,
                                        std::uint8_t kind) noexcept {
    if (!complete_native_player_profile(profile)) {
        return false;
    }
    if (!writer.write(value, kProfileScalarWidth)
        || !writer.write(kind + kPlayerKindBias, kProfileSmallWidth)) {
        return false;
    }
    if (!write_native_player_profile(writer, profile)) {
        return false;
    }
    // Sub-block A's own name slot must byte-match the one just written for B.
    if (!writer.write(1U, kFlagWidth) || !write_profile_name(writer, profile)
        || !writer.write(1U, kFlagWidth) || !writer.write(kProfileSubAWire, kProfileSubA23Width)
        || !writer.write(1U, kFlagWidth) || !writer.write(kProfileSubAWire, kProfileSubA23Width)
        || !writer.write(1U, kFlagWidth) || !writer.write(kProfileSubAWire, kProfileSubA4FirstWidth)
        || !writer.write(kProfileSubAWire, kProfileSubA4SecondWidth)) {
        return false;
    }
    return write_native_player_tail(writer, profile);
}

/**
 * Writes one player-delta entry carrying an identity and, when the row has one, a profile group.
 * @param writer Open writer.
 * @param player Player row to publish.
 * @return True when every field fit.
 */
[[nodiscard]] bool write_player_delta(bits::Writer& writer,
                                      const MembershipPlayer& player) noexcept {
    if (!writer.write(player.slot, kPlayerIndexWidth) || !writer.write(kDeltaEntryFull, kFlagWidth)
        || !writer.write(1U, kFlagWidth) || !bits::write_raw_u64(writer, player.playerId)
        || !writer.write(player.memberIndex, kPlayerMemberWidth)
        || !writer.write(kPlayerOwnedIndexZero, kPlayerOwnedIndexWidth)
        || !writer.write(player.addSequence, kPlayerSequenceWidth)
        || !writer.write(player.flag ? 1U : 0U, kFlagWidth)
        || !writer.write(player.hasProfile ? 1U : 0U, kFlagWidth)) {
        return false;
    }
    if (!player.hasProfile) {
        return true;
    }
    NativePlayerProfile profile = player.nativeProfile;
    profile.soids.present = true;
    profile.soids.accountSoid = player.accountSoid;
    profile.soids.characterSoid = player.characterSoid;
    return write_player_profile(writer, profile, player.profileValue, player.profileKind);
}

} // namespace

/** Writes a peer-connect body. */
bool write_peer_connect(bits::Writer& writer, const PeerConnect& body) noexcept {
    return writer.write(body.protocolVersion, kProtocolWidth)
           && bits::write_raw_u64(writer, body.machineId)
           && bits::write_raw_u64(writer, body.sessionId);
}

/** Writes a join-complete body. */
bool write_join_complete(bits::Writer& writer, const JoinComplete& body) noexcept {
    return bits::write_raw_u64(writer, body.sessionId)
           && bits::write_raw_u64(writer, body.machineId)
           && writer.write(body.joinSequence, kSequenceWidth);
}

/** Reads a join-complete body. */
bool read_join_complete(bits::Reader& reader, JoinComplete& output) noexcept {
    JoinComplete candidate{};
    std::uint64_t sequence = 0;
    if (!bits::read_raw_u64(reader, candidate.sessionId)
        || !bits::read_raw_u64(reader, candidate.machineId)
        || !reader.read(kSequenceWidth, sequence)) {
        return false;
    }
    candidate.joinSequence = static_cast<std::uint32_t>(sequence);
    output = candidate;
    return true;
}

/** Reads a join-abort body. */
bool read_join_abort(bits::Reader& reader, SessionNotice& output) noexcept {
    std::uint64_t flag = 0;
    SessionNotice candidate{};
    if (!bits::read_raw_u64(reader, candidate.sessionId)
        || !bits::read_raw_u64(reader, candidate.machineId) || !reader.read(kFlagWidth, flag)) {
        return false;
    }
    candidate.flag = flag != 0;
    output = candidate;
    return true;
}

/** Reads a session-identity-only body. */
bool read_session_only(bits::Reader& reader, std::uint64_t& output) noexcept {
    return bits::read_raw_u64(reader, output);
}

/** Writes a session-identity-only body. */
bool write_session_only(bits::Writer& writer, std::uint64_t sessionId) noexcept {
    return bits::write_raw_u64(writer, sessionId);
}

/** Writes a session-disband body. */
bool write_session_disband(bits::Writer& writer, const SessionNotice& body) noexcept {
    return bits::write_raw_u64(writer, body.sessionId)
           && bits::write_raw_u64(writer, body.machineId)
           && writer.write(body.flag ? 1U : 0U, kFlagWidth);
}

/** Writes a session-boot body. */
bool write_session_boot(bits::Writer& writer, const SessionBoot& body) noexcept {
    return bits::write_raw_u64(writer, body.sessionId) && writer.write(body.kind, kBootKindWidth)
           && writer.write(body.reason, kBootReasonWidth)
           && bits::write_raw_u64(writer, body.machineId);
}

/** Reads a time-synchronize body. */
bool read_time_synchronize(bits::Reader& reader, TimeSynchronize& output) noexcept {
    std::uint64_t variant = 0;
    TimeSynchronize candidate{};
    if (!bits::read_raw_u64(reader, candidate.sessionId) || !reader.read(kFlagWidth, variant)
        || !reader.read(kSampleWidth, candidate.requesterSendTime)) {
        return false;
    }
    candidate.threeSample = variant != 0;
    if (candidate.threeSample
        && (!reader.read(kSampleWidth, candidate.responderReceiveTime)
            || !reader.read(kSampleWidth, candidate.responderSendTime))) {
        return false;
    }
    output = candidate;
    return true;
}

/** Writes a time-synchronize body. */
bool write_time_synchronize(bits::Writer& writer, const TimeSynchronize& body) noexcept {
    if (!bits::write_raw_u64(writer, body.sessionId)
        || !writer.write(body.threeSample ? 1U : 0U, kFlagWidth)
        || !writer.write(body.requesterSendTime, kSampleWidth)) {
        return false;
    }
    if (!body.threeSample) {
        return true;
    }
    return writer.write(body.responderReceiveTime, kSampleWidth)
           && writer.write(body.responderSendTime, kSampleWidth);
}

/** Writes a complete-snapshot membership update. */
bool write_membership_update(bits::Writer& writer, const MembershipUpdate& body) noexcept {
    // The consumer refuses the message unless the base revision is below the message revision,
    // and a complete snapshot always publishes base revision 0.
    if (body.revision == 0 || body.members.size() > kMemberCapacity
        || body.players.size() > kPlayerCapacity) {
        return false;
    }
    for (const MembershipPlayer& player : body.players) {
        if (player.slot >= kPlayerCapacity || player.memberIndex >= kMemberCapacity
            || player.profileKind > kPlayerKindMaximum
            || (player.hasProfile && !valid_native_player_profile(player.nativeProfile))) {
            return false;
        }
    }
    std::array<std::byte, kProtobufCapacity> protobufStorage{};
    std::size_t protobufSize = 0;
    if (!write_membership_protobuf(body, protobufStorage, protobufSize)) {
        return false;
    }
    if (!bits::write_raw_u64(writer, body.hostMachineId)
        || !writer.write(protobufSize, kProtobufLengthWidth)
        || !bits::write_raw(writer, {protobufStorage.data(), protobufSize})
        || !writer.write(kRevisionPairPresent, kFlagWidth)
        || !writer.write(kCompleteSnapshotBase, kRevisionWidth)
        || !writer.write(kUnreadWord, kRevisionWidth)
        || !writer.write(body.members.size(), kDeltaCountWidth)
        || !writer.write(body.players.size(), kDeltaCountWidth)) {
        return false;
    }
    for (std::size_t index = 0; index < body.members.size(); ++index) {
        if (!write_peer_delta(writer, index, body.members[index])) {
            return false;
        }
    }
    for (const MembershipPlayer& player : body.players) {
        if (!write_player_delta(writer, player)) {
            return false;
        }
    }
    for (std::size_t group = 0; group < kTailGroupCount; ++group) {
        if (!writer.write(kTailGroupAbsent, kFlagWidth)) {
            return false;
        }
    }
    // The consumer hashes its own state after applying and compares. The replica layout must
    // stay in step with it.
    return writer.write(session_state_hash(body), kWordWidth);
}

} // namespace sunrise::middleware::gameplay::group
