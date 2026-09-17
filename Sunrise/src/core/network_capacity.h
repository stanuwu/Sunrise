#pragma once

#include <cstddef>

namespace sunrise::core::network_capacity {
/** Server-wide enrollment ceiling, independent of native per-activity party limits. */
inline constexpr std::size_t kPlayers = 32;
/** Existing native group-host advertisement limit; activity-specific limits may be lower. */
inline constexpr std::size_t kActivityPlayers = 12;
/** Lobby plus overlapping current/target native activity connections for each player. */
inline constexpr std::size_t kConnectionsPerPlayer = 4;
/** Every player's connections, summed for tables sized to the whole server. */
inline constexpr std::size_t kConnections = kPlayers * kConnectionsPerPlayer;
/** Two complete eight-region directories per player, including travel overlap. */
inline constexpr std::size_t kHostPorts = kPlayers * 16;
/** Host records plus each player's current and target private activity. */
inline constexpr std::size_t kActivitySessions = kHostPorts + kPlayers * 2;
} // namespace sunrise::core::network_capacity
