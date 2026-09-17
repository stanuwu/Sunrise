#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../state/gameplay/definition.h"

namespace sunrise::server::gameplay::relay {
/** Registrations belong to native BAP links; travel may overlap several links per player. */
inline constexpr std::size_t kClientCapacity = core::network_capacity::kConnections;
/** One full player mesh per overlapping native link, without sharing pair lifetimes. */
inline constexpr std::size_t kPairCapacity = core::network_capacity::kPlayers
                                             * (core::network_capacity::kPlayers - 1) / 2
                                             * core::network_capacity::kConnectionsPerPlayer;
/** A relay pair is exactly the two endpoints it forwards between. */
inline constexpr std::size_t kPairMemberCapacity = 2;
/** Every relayed datagram opens with its four-byte session id. */
inline constexpr std::size_t kFramingHeaderSize = 4;
/** A native registration starts alone; only an authenticated introduction may pair it. */
[[nodiscard]] std::uint32_t register_client(std::uint32_t connection,
                                            const state::gameplay::Endpoint& endpoint) noexcept;
/**
 * Pairs two already-registered connections under one relay session, if not already paired.
 * @return True once paired (already, or newly). False for a zero or matching connection, an
 * unregistered one, or no free pair slot or session id available.
 */
[[nodiscard]] bool pair_clients(std::uint32_t first, std::uint32_t second) noexcept;
/**
 * @param peer Zero resolves `connection`'s current session: its own registration identity while
 * unpaired, or its pair's shared session once paired with exactly one peer.
 * @return The session id, or zero when `connection` is unregistered, `peer` is not paired with
 * it, or (with `peer` zero) `connection` is paired with more than one peer.
 */
[[nodiscard]] std::uint32_t session_id_of(std::uint32_t connection,
                                          std::uint32_t peer = 0) noexcept;
/** @return The connection paired with `connection`, or zero when unpaired or ambiguous. */
[[nodiscard]] std::uint32_t peer_connection_of(std::uint32_t connection) noexcept;
/** Keeps the other native registration available, with a fresh unpaired session id. */
void release_client(std::uint32_t connection) noexcept;
/** Clears the pairing between `first` and `second`, leaving both registrations intact. */
void release_pair(std::uint32_t first, std::uint32_t second) noexcept;
/** Forwards the complete native datagram only within its registered pair. */
[[nodiscard]] bool route(const state::gameplay::Endpoint& from,
                         std::span<const std::byte> datagram) noexcept;
/** Clears every registration and pairing. */
void reset() noexcept;
} // namespace sunrise::server::gameplay::relay
