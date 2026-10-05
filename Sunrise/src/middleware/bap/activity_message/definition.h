#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "telemetry.h"

namespace sunrise::middleware::bap::activity_message {

/** Discriminator 1 carries the peer-heard mask and is the only variant this client sends. */
inline constexpr std::byte kTransportDiscriminator{1};
/** Discriminator 2 omits the peer-heard mask but is accepted by the native receiver. */
inline constexpr std::byte kCompactTransportDiscriminator{2};
/** The activity decoder rejects payloads larger than 0x7D800 bytes. */
inline constexpr std::size_t kMaximumPayloadSize = 0x7D800;
/** Zero cannot name an allocated activity session. */
inline constexpr std::uint64_t kAbsentSessionId = 0;
/** FNV-1 32-bit offset basis. The client hashes no string to it, so it is its empty-name value. */
inline constexpr std::uint32_t kEmptyNameHash = 0x811C9DC5;

/** Exact service-8 body arm selected by its leading discriminator. */
enum class RequestVariant : std::uint8_t {
    none,
    withPeerHeardMask = 1,
    withoutPeerHeardMask = 2,
};

/** Checked service-8 envelope fields, borrowed from caller-owned storage. */
struct Request final {
    /** Activity session the link owns. It equals the id the svc-17 handoff named. */
    std::uint64_t sessionId{};
    std::uint32_t messageType{};
    std::uint32_t peerHeardMask{};
    RequestVariant variant{RequestVariant::none};
    /** Sensitive payload. Never keep, log, capture, cache or save this view. */
    std::span<const std::byte> payload{};
};

/** Typed values needed from the fixed prefix of an activity join request. */
struct JoinRequest final {
    /** Native BC identity, including account and player key supplied by this client. */
    telemetry::ReservationRecord identity{};
    std::uint32_t correlation{};
    std::uint64_t sessionId{};
    /** 8 wire bytes, low byte first, that name this client inside membership State. */
    std::uint64_t memberKey{};
    /**
     * The character the client signed in on. The roster's participation key must be this
     * character: the client binds the player by matching it, so another character of the same
     * account binds nothing.
     */
    std::uint64_t characterSoid{};
};

} // namespace sunrise::middleware::bap::activity_message
