#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../encoding/bit_reader.h"
#include "../descriptor/join_descriptor.h"

namespace sunrise::middleware::gameplay::group {

/**
 * Registry ids of the group messages a host normally emits.
 * Receiving one means the peer is acting as the authority for that group. Nothing here acts on
 * them, but each is read so the container behind it stays readable.
 */
enum class NoticeMessageId : std::uint8_t {
    peerConnectNotice = 11,
    sessionBootNotice = 18,
    delegateLeadership = 32,
    bootMachine = 33,
    playerRefuse = 35,
    voiceRegistration = 41,
};

/** Declared decoded sizes the registry holds for those ids. */
inline constexpr std::uint32_t kPeerConnectNoticeSize = 24;
/** See kPeerConnectNoticeSize. */
inline constexpr std::uint32_t kDelegateLeadershipSize = 94;
/** See kPeerConnectNoticeSize. */
inline constexpr std::uint32_t kBootMachineSize = 100;
/** See kPeerConnectNoticeSize. */
inline constexpr std::uint32_t kPlayerRefuseSize = 24;
/** See kPeerConnectNoticeSize. */
inline constexpr std::uint32_t kVoiceRegistrationSize = 16;

/** The voice registration is 128 opaque bits the checked build never reads. */
inline constexpr std::size_t kVoiceRegistrationBytes = 16;

/** Body of a peer-connect notice. */
struct PeerConnectNotice {
    std::uint16_t protocolVersion{};
    std::uint64_t machineId{};
    std::uint64_t sessionId{};
};

/** Body of a session-boot notice. */
struct SessionBootNotice {
    std::uint64_t sessionId{};
    std::uint64_t machineId{};
    std::uint8_t kind{};
    std::uint8_t reason{};
};

/** Body of a leadership delegation or a machine boot. Only the boot carries a kind. */
struct AddressedNotice {
    std::uint64_t sessionId{};
    std::array<std::byte, descriptor::kNetAddrSize> address{};
    std::uint8_t kind{};
    bool hasKind{};
};

/** Body of a player refusal. */
struct PlayerRefuse {
    std::uint64_t sessionId{};
    std::uint64_t playerId{};
    std::uint8_t reason{};
    bool kindFlag{};
};

/** Reads a peer-connect notice. @return True when every field was present. */
[[nodiscard]] bool read_peer_connect(encoding::bits::Reader& reader,
                                     PeerConnectNotice& output) noexcept;

/** Reads a session-boot notice. @return True when every field was present. */
[[nodiscard]] bool read_session_boot(encoding::bits::Reader& reader,
                                     SessionBootNotice& output) noexcept;

/**
 * Reads a leadership delegation or a machine boot.
 * @param reader Reader positioned at the body.
 * @param withKind True for the boot, which carries a kind between the session and the address.
 * @param output Receives the fields.
 * @return True when every field was present.
 */
[[nodiscard]] bool read_addressed_notice(encoding::bits::Reader& reader,
                                         bool withKind,
                                         AddressedNotice& output) noexcept;

/** Reads a player refusal. @return True when every field was present. */
[[nodiscard]] bool read_player_refuse(encoding::bits::Reader& reader,
                                      PlayerRefuse& output) noexcept;

/** Consumes the opaque voice registration. @return True when the whole field was present. */
[[nodiscard]] bool read_voice_registration(encoding::bits::Reader& reader) noexcept;

} // namespace sunrise::middleware::gameplay::group
