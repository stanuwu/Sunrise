#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../state/gameplay/definition.h"

namespace sunrise::server::gameplay::endpoint {

/**
 * Binds the gameplay UDP endpoint when the configured topology is embedded.
 * External topology binds no socket, because another process owns it. Disabled topology succeeds
 * and does nothing.
 * @return True when the configured topology is ready to be advertised.
 */
[[nodiscard]] bool initialize() noexcept;

/**
 * Drains a bounded number of datagrams and expires stale associations.
 * @param now Monotonic tick count in milliseconds.
 */
void service(std::uint64_t now) noexcept;

/**
 * Sends one datagram to a client endpoint.
 * @param destination Host-order address and port.
 * @param datagram Complete datagram bytes.
 * @return True when the whole datagram was handed to Winsock.
 */
[[nodiscard]] bool send_to(const state::gameplay::Endpoint& destination,
                           std::span<const std::byte> datagram) noexcept;

/** Closes the socket and releases the Winsock reference this module took. */
void shutdown() noexcept;

/** @return True once an embedded endpoint is bound, or external topology is preflighted. */
[[nodiscard]] bool ready() noexcept;

/** @return Endpoint published in the join descriptor. Zero when the channel is disabled. */
[[nodiscard]] state::gameplay::Endpoint advertised() noexcept;

/** Dedicated relay endpoint, zero unless the relay socket/topology is available. */
[[nodiscard]] state::gameplay::Endpoint relay_endpoint() noexcept;

/**
 * Reports the host port one activity-host row advertises.
 * Every row takes its own port. So the client never shares one channel between two hosts, and
 * never has to swap a security association in place when it moves between them.
 * @param row Activity-host row index.
 * @return That row's bound port, or the primary port when the row is out of range.
 */
[[nodiscard]] std::uint16_t host_port(std::size_t row) noexcept;

/** Identity the join descriptor publishes beside the endpoint. */
struct Identity {
    /** Nonzero machine identity. A zero identity makes the descriptor read as absent. */
    std::uint64_t machineId{};
    /** Nonzero online session id, which is also the lobby the client is told to join. */
    std::uint64_t onlineSessionId{};
};

/** @return Identity generated when the endpoint bound, or zeroes when it did not. */
[[nodiscard]] Identity identity() noexcept;

} // namespace sunrise::server::gameplay::endpoint
