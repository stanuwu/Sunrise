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
inline constexpr std::size_t kPairMemberCapacity = 2;
inline constexpr std::size_t kFramingHeaderSize = 4;
/** A native registration starts alone; only an authenticated introduction may pair it. */
[[nodiscard]] std::uint32_t register_client(std::uint32_t connection,
                                            const state::gameplay::Endpoint& endpoint) noexcept;
[[nodiscard]] bool pair_clients(std::uint32_t first, std::uint32_t second) noexcept;
[[nodiscard]] std::uint32_t session_id_of(std::uint32_t connection,
                                          std::uint32_t peer = 0) noexcept;
[[nodiscard]] std::uint32_t peer_connection_of(std::uint32_t connection) noexcept;
/** Keeps the other native registration available, with a fresh unpaired session id. */
void release_client(std::uint32_t connection) noexcept;
void release_pair(std::uint32_t first, std::uint32_t second) noexcept;
/** Forwards the complete native datagram only within its registered pair. */
[[nodiscard]] bool route(const state::gameplay::Endpoint& from,
                         std::span<const std::byte> datagram) noexcept;
void reset() noexcept;
} // namespace sunrise::server::gameplay::relay
