#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../encoding/bit_reader.h"
#include "../../encoding/bit_writer.h"
#include "../descriptor/join_descriptor.h"
#include "native_player_profile.h"

namespace sunrise::middleware::gameplay::group {

/** Registry ids of the group-session messages this host implements. */
enum class SessionMessageId : std::uint8_t {
    peerConnect = 11,
    membershipUpdate = 30,
    joinComplete = 12,
    joinAbort = 13,
    leaveSession = 15,
    leaveAcknowledge = 16,
    sessionDisband = 17,
    sessionBoot = 18,
    peerEstablish = 26,
    timeSynchronize = 29,
};

/** Declared decoded sizes the registry holds. A message must carry its own constant. */
inline constexpr std::uint32_t kPeerConnectSize = 24;
/** See kPeerConnectSize. */
inline constexpr std::uint32_t kJoinCompleteSize = 24;
/** See kPeerConnectSize. */
inline constexpr std::uint32_t kJoinAbortSize = 24;
/** See kPeerConnectSize. */
inline constexpr std::uint32_t kLeaveSessionSize = 8;
/** See kPeerConnectSize. */
inline constexpr std::uint32_t kLeaveAcknowledgeSize = 8;
/** See kPeerConnectSize. */
inline constexpr std::uint32_t kSessionDisbandSize = 24;
/** See kPeerConnectSize. */
inline constexpr std::uint32_t kSessionBootSize = 24;
/** See kPeerConnectSize. */
inline constexpr std::uint32_t kPeerEstablishSize = 8;
/** See kPeerConnectSize. */
inline constexpr std::uint32_t kTimeSynchronizeSize = 48;
/** See kPeerConnectSize. */
inline constexpr std::uint32_t kMembershipUpdateSize = 31104;

/** Member slots one session holds. It is both the schema array bound and the delta index range. */
inline constexpr std::size_t kMemberCapacity = 32;

/** Body of a peer-connect fan-out. */
struct PeerConnect {
    std::uint16_t protocolVersion{};
    std::uint64_t machineId{};
    std::uint64_t sessionId{};
};

/** Body of a join-complete. */
struct JoinComplete {
    std::uint64_t sessionId{};
    std::uint64_t machineId{};
    std::uint32_t joinSequence{};
};

/** Body shared by join-abort and session-disband: two identities and one flag. */
struct SessionNotice {
    std::uint64_t sessionId{};
    std::uint64_t machineId{};
    bool flag{};
};

/** Body of a session-boot. */
struct SessionBoot {
    std::uint64_t sessionId{};
    std::uint64_t machineId{};
    std::uint8_t kind{};
    std::uint8_t reason{};
};

/** Body of a time-synchronize probe or response. */
struct TimeSynchronize {
    std::uint64_t sessionId{};
    /** True selects the three-sample reply. False is the one-sample request. */
    bool threeSample{};
    /** Requester's millisecond clock at send. The reply echoes it. */
    std::uint64_t requesterSendTime{};
    /** Responder's millisecond clock at receive. Reply only. */
    std::uint64_t responderReceiveTime{};
    /** Responder's millisecond clock at send. Reply only. */
    std::uint64_t responderSendTime{};
};

/** Writes a peer-connect body. @return True when every field fit. */
[[nodiscard]] bool write_peer_connect(encoding::bits::Writer& writer,
                                      const PeerConnect& body) noexcept;

/** Writes a join-complete body. @return True when every field fit. */
[[nodiscard]] bool write_join_complete(encoding::bits::Writer& writer,
                                       const JoinComplete& body) noexcept;

/** Reads a join-complete body. @return True when every field was present. */
[[nodiscard]] bool read_join_complete(encoding::bits::Reader& reader,
                                      JoinComplete& output) noexcept;

/** Reads a join-abort body. @return True when every field was present. */
[[nodiscard]] bool read_join_abort(encoding::bits::Reader& reader, SessionNotice& output) noexcept;

/** Reads a session-identity-only body, shared by leave, acknowledge, and establish. */
[[nodiscard]] bool read_session_only(encoding::bits::Reader& reader,
                                     std::uint64_t& output) noexcept;

/** Writes a session-identity-only body. @return True when the field fit. */
[[nodiscard]] bool write_session_only(encoding::bits::Writer& writer,
                                      std::uint64_t sessionId) noexcept;

/** Writes a session-disband body. @return True when every field fit. */
[[nodiscard]] bool write_session_disband(encoding::bits::Writer& writer,
                                         const SessionNotice& body) noexcept;

/** Writes a session-boot body. @return True when every field fit. */
[[nodiscard]] bool write_session_boot(encoding::bits::Writer& writer,
                                      const SessionBoot& body) noexcept;

/** Reads a time-synchronize body. @return True when the selected form was complete. */
[[nodiscard]] bool read_time_synchronize(encoding::bits::Reader& reader,
                                         TimeSynchronize& output) noexcept;

/** Writes a time-synchronize body. @return True when the selected form fit. */
[[nodiscard]] bool write_time_synchronize(encoding::bits::Writer& writer,
                                          const TimeSynchronize& body) noexcept;

/**
 * Peer state a membership entry publishes. It is a ladder, and the decoder refuses any value
 * above `established`. Only the values this host uses are named.
 */
enum class MemberState : std::uint8_t {
    /** Reserved for an ambassador. Every occupancy check disqualifies this slot. */
    reservedAmbassador = 3,
    /** Reached once a channel is up. It is below the bar a joining peer sets for itself. */
    connected = 5,
    /** Smallest value a joining peer accepts for its own entry before it finishes its join. */
    joined = 7,
    /** Smallest value that keeps a member in the lists the consumer reports. */
    waiting = 8,
    /** Highest value below `established`, and what this host publishes for a joining peer. */
    ready = 9,
    /** End of the ladder. A joining peer whose own entry reads this ends its own join request. */
    established = 10,
};

/** One member row of a membership snapshot. */
struct MembershipMember {
    /** NetAddr the member is reached at. The consumer refuses a zero address. */
    std::array<std::byte, descriptor::kNetAddrSize> address{};
    /** Machine identity, published as eight bytes in memory order. */
    std::uint64_t machineId{};
    /** Join id the peer sent in its join request. The peer looks itself up by address and refuses
     *  the update when this does not echo the id its own join carries. Zero is the empty id. */
    std::uint64_t joinId{};
    MemberState state{MemberState::established};
    /** True publishes the three values below and makes the consumer resolve the peer link. */
    bool connectionPresent{};
    std::uint32_t joinCompatibility{};
    std::uint64_t joinTimestamp{};
    /** The third connection value has no known name; zero is its cleared value. */
    std::uint8_t connectionValue{};
    /** True publishes the player slot below. A member with no player publishes none. */
    bool ownsPlayerSlot{};
    std::uint32_t playerSlot{};
};

/** Player slots one session holds. It is both the table bound and the delta index range. */
inline constexpr std::size_t kPlayerCapacity = 32;

/**
 * One player row of a membership snapshot.
 * The fields are the ones the consumer's own local add writes, in the same order.
 */
struct MembershipPlayer {
    /** Player slot the row occupies. */
    std::uint32_t slot{};
    /** Player identity the owning member sent in its player-add. */
    std::uint64_t playerId{};
    /** Member index that owns the player. */
    std::uint32_t memberIndex{};
    /** Session player-add counter at the time of the add. The first player of a session gets 0. */
    std::uint32_t addSequence{};
    /** One-bit field the local add takes from its caller. Zero is its cleared value. */
    bool flag{};
    /** True publishes the profile group below. Without it the peer's player has no account soid
     *  and its own simulation refuses to create the player. */
    bool hasProfile{};
    /** Value the profile group leads with. Zero is the receiver's cleared value. */
    std::uint32_t profileValue{};
    /** Player kind, 0 through 3. The wire carries it plus one in three bits. */
    std::uint8_t profileKind{};
    /** Account soid the profile block's `+192` field carries. */
    std::uint64_t accountSoid{};
    /** Character soid the same field carries second. */
    std::uint64_t characterSoid{};
    /** Profile retained from this player's native add and sparse publications. */
    NativePlayerProfile nativeProfile{};
};

/** Complete membership snapshot one host publishes. */
struct MembershipUpdate {
    /** Host machine identity, written ahead of the protobuf. */
    std::uint64_t hostMachineId{};
    /** Membership revision. It must be at least 1 and must strictly increase. */
    std::uint32_t revision{};
    /** Member index of the host. */
    std::uint32_t hostMemberIndex{};
    /** Assumed to be the member index the host nominates to succeed it. */
    std::uint32_t successionIndex{};
    /** Members in index order, starting at index 0. */
    std::span<const MembershipMember> members{};
    /** Players the snapshot publishes. Each names the member that owns it. */
    std::span<const MembershipPlayer> players{};
};

/**
 * Writes a complete-snapshot membership update.
 * The receiving peer must be in `members`. It clears its own table and finds itself by NetAddr.
 * The trailing hash covers the state it will hold after applying; `session_state.h` builds it.
 * @param writer Writer positioned at the body.
 * @param body Snapshot to publish.
 * @return True when the whole body fit and the revision and member count are encodable.
 */
[[nodiscard]] bool write_membership_update(encoding::bits::Writer& writer,
                                           const MembershipUpdate& body) noexcept;

/** Native unassigned sub-block-A scalar encoding (record bytes are all -1).
 * The native add initializes these bytes to -1; the name comes from the owning player. */
inline constexpr std::uint64_t kProfileSubAWire = 0;

} // namespace sunrise::middleware::gameplay::group
