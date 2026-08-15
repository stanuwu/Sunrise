#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::activity_message {

/** Only discriminator 1 carries the supported join envelope; variant 2 is rejected. */
inline constexpr std::byte kTransportDiscriminator{1};
/** The activity decoder rejects payloads larger than 0x7D800 bytes. */
inline constexpr std::size_t kMaximumPayloadSize = 0x7D800;
/** Zero cannot name an allocated activity session. */
inline constexpr std::uint64_t kAbsentSessionId = 0;

/** Checked service-8 envelope fields, borrowed from caller-owned storage. */
struct Request final {
    std::uint64_t accountHandle{};
    std::uint32_t messageType{};
    std::uint32_t peerHeardMask{};
    /** Sensitive payload. Never keep, log, capture, cache or save this view. */
    std::span<const std::byte> payload{};
};

/** Typed values needed from the fixed prefix of an activity join request. */
struct JoinRequest final {
    std::uint32_t correlation{};
    std::uint64_t sessionId{};
    /** 8 wire bytes, low byte first, that name this client inside membership State. */
    std::uint64_t memberKey{};
    /**
     * The character the client signed in on, or zero when the payload stops short of it. The
     * roster's participation key must be this character: the client binds the player by matching
     * it, so a different character of the same account binds nothing.
     */
    std::uint64_t characterSoid{};
};

} // namespace sunrise::middleware::bap::activity_message
