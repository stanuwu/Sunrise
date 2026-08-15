#pragma once

#include <WinSock2.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../client/network/consumer.h"

namespace sunrise::server::transport {

/** One outer frame is a magic byte, a type byte, and a big-endian payload length. */
inline constexpr std::size_t kOuterHeaderSize = 6;
/** The payload length is the 4 bytes after the magic and type. */
inline constexpr std::size_t kOuterLengthOffset = 2;
/** Owed pushes are checked at this rate while socket I/O stays per callback. */
inline constexpr int kServiceIntervalMs = 50;
/** One connection buffers at most this many streamed bytes before a frame must complete. */
inline constexpr std::size_t kStreamCapacity = client::network::kBapFrameCapacity;

/** One accepted connection with bounded ingress and committed egress storage. */
struct Peer {
    SOCKET socket{INVALID_SOCKET};
    std::size_t streamSize{};
    std::array<std::byte, kStreamCapacity> stream{};
    std::size_t outputOffset{};
    std::size_t outputSize{};
    std::array<std::byte, client::network::kBapFrameCapacity> output{};
};

/** Nonblocking listener state serviced only while the lifecycle lock is held. */
struct Listener {
    bool active{};
    bool winsockOwned{};
    SOCKET acceptor{INVALID_SOCKET};
    std::uint64_t nextPollTick{};
    std::array<Peer, client::network::kBapConnectionCount> peers{};
};

extern Listener g_listener;

/** A peer slot answers on the connection id the Server indexes its sessions by. */
[[nodiscard]] constexpr std::uint32_t connection_id(std::size_t slot) noexcept {
    return static_cast<std::uint32_t>(slot + 1);
}

/**
 * Offers one event to the Server and stages whatever it produces.
 * @param frame Whole inbound frame, or empty for a lifecycle event.
 * @return True when the response metadata fits the peer buffer.
 */
[[nodiscard]] bool
offer(std::size_t slot, client::network::BapEvent event, std::span<const std::byte> frame) noexcept;

/**
 * Removes and offers at most one whole frame from one peer's stream.
 * @return True while the buffered prefix is valid.
 */
[[nodiscard]] bool drain_stream(std::size_t slot) noexcept;

/**
 * Advances one committed output by an accepted send count.
 * @param peer Peer whose output is pending.
 * @param sent Positive byte count Winsock accepted.
 * @return True when the count fits the pending suffix.
 */
[[nodiscard]] bool advance_output(Peer& peer, std::size_t sent) noexcept;

/** Closes one peer and reports its session end to the Server. */
void close_peer(std::size_t slot) noexcept;

/** @param port Host-order loopback port. Zero picks an ephemeral port. */
[[nodiscard]] bool initialize_on_port(std::uint16_t port) noexcept;

} // namespace sunrise::server::transport
