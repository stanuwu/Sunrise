/**
 * Bodies a group host emits. This host reads them so a container carrying one stays readable, and
 * acts on none of them: each one names an authority decision that belongs to whichever peer holds
 * the group, and answering from a body alone is how two peers end up both acting as host.
 */

#include "notice_messages.h"

#include "../../encoding/bit_raw.h"

namespace sunrise::middleware::gameplay::group {

namespace {

namespace bits = encoding::bits;

/** The protocol version is a 16-bit value field. */
constexpr std::uint8_t kProtocolVersionWidth = 16;
/** The boot kind is three bits. */
constexpr std::uint8_t kBootKindWidth = 3;
/** The boot reason is five bits. */
constexpr std::uint8_t kBootReasonWidth = 5;
/** The refusal kind is one bit. */
constexpr std::uint8_t kRefuseKindWidth = 1;
/** The refusal reason is six bits. */
constexpr std::uint8_t kRefuseReasonWidth = 6;

} // namespace

/** Reads a peer-connect notice. */
bool read_peer_connect(bits::Reader& reader, PeerConnectNotice& output) noexcept {
    PeerConnectNotice candidate{};
    std::uint64_t version = 0;
    if (!reader.read(kProtocolVersionWidth, version)
        || !bits::read_raw_u64(reader, candidate.machineId)
        || !bits::read_raw_u64(reader, candidate.sessionId)) {
        return false;
    }
    candidate.protocolVersion = static_cast<std::uint16_t>(version);
    output = candidate;
    return true;
}

/** Reads a session-boot notice. */
bool read_session_boot(bits::Reader& reader, SessionBootNotice& output) noexcept {
    SessionBootNotice candidate{};
    std::uint64_t kind = 0;
    std::uint64_t reason = 0;
    if (!bits::read_raw_u64(reader, candidate.sessionId) || !reader.read(kBootKindWidth, kind)
        || !reader.read(kBootReasonWidth, reason)
        || !bits::read_raw_u64(reader, candidate.machineId)) {
        return false;
    }
    candidate.kind = static_cast<std::uint8_t>(kind);
    candidate.reason = static_cast<std::uint8_t>(reason);
    output = candidate;
    return true;
}

/** Reads a leadership delegation or a machine boot. */
bool read_addressed_notice(bits::Reader& reader, bool withKind, AddressedNotice& output) noexcept {
    AddressedNotice candidate{};
    if (!bits::read_raw_u64(reader, candidate.sessionId)) {
        return false;
    }
    if (withKind) {
        std::uint64_t kind = 0;
        if (!reader.read(kBootKindWidth, kind)) {
            return false;
        }
        candidate.kind = static_cast<std::uint8_t>(kind);
        candidate.hasKind = true;
    }
    if (!bits::read_raw(reader, candidate.address)) {
        return false;
    }
    output = candidate;
    return true;
}

/** Reads a player refusal. */
bool read_player_refuse(bits::Reader& reader, PlayerRefuse& output) noexcept {
    PlayerRefuse candidate{};
    std::uint64_t kind = 0;
    std::uint64_t reason = 0;
    if (!bits::read_raw_u64(reader, candidate.sessionId)
        || !bits::read_raw_u64(reader, candidate.playerId) || !reader.read(kRefuseKindWidth, kind)
        || !reader.read(kRefuseReasonWidth, reason)) {
        return false;
    }
    candidate.kindFlag = kind != 0;
    candidate.reason = static_cast<std::uint8_t>(reason);
    output = candidate;
    return true;
}

/** Consumes the opaque voice registration. */
bool read_voice_registration(bits::Reader& reader) noexcept {
    // The checked build only probes whether the voice singleton exists and never reads the body.
    return bits::skip_raw(reader, kVoiceRegistrationBytes);
}

} // namespace sunrise::middleware::gameplay::group
