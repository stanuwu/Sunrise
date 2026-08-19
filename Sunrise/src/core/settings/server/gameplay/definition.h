#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::core::settings::server::gameplay {

/** Process that owns the gameplay UDP endpoint for this run. */
enum class Topology : std::uint8_t {
    /** No endpoint is bound and no method-0 descriptor is advertised. */
    disabled,
    /** Sunrise binds the endpoint and hosts the peer protocol itself. */
    embedded,
    /** A configured process owns the endpoint. Sunrise binds nothing. */
    external,
};

/** Octets in one IPv4 address. */
inline constexpr std::size_t kAddressOctets = 4;
/** The join descriptor carries an even UDP port, so an odd port is refused at load. */
inline constexpr std::uint16_t kPortAlignment = 2;
/** Even, and clear of both discovery ports. */
inline constexpr std::uint16_t kDefaultPort = 30976;
/** Discovery owns 3074 and 3075, so the gameplay endpoint may take neither. */
inline constexpr std::uint16_t kDiscoveryPortLow = 3074;
/** Upper discovery port. See kDiscoveryPortLow. */
inline constexpr std::uint16_t kDiscoveryPortHigh = 3075;
/** One carrier, the live wave, and removal headroom fit in this many reserved slots. */
inline constexpr std::uint16_t kDefaultServerReserve = 256;
/** A smaller reserve cannot hold one carrier plus its removal and quarantine headroom. */
inline constexpr std::uint16_t kMinimumServerReserve = 8;
/** The client keeps at least this many lease bits after the reserve is subtracted. */
inline constexpr std::uint16_t kClientLeaseMinimum = 4096;
/**
 * Entity slots the join hands the client before it asks for any.
 * The whole slot space, capped at what the reserve leaves, which is the measured behaviour.
 */
inline constexpr std::uint16_t kDefaultClientJoinGrant = 8'192;
/** Below this a join cannot cover the client's own low water mark of 400. */
inline constexpr std::uint16_t kMinimumClientJoinGrant = 400;

/**
 * Gameplay endpoint topology and the entity-slot split it implies.
 * The advertised address is the one message 12 publishes. The transport address is where the
 * datagram lands after the Client's egress rewrite. Both carry the same port.
 */
struct Settings {
    /** Embedded by default. Public activities reach each other over the peer protocol. */
    Topology topology{Topology::embedded};
    /** Local interface the embedded endpoint binds. */
    std::array<unsigned char, kAddressOctets> bindAddress{127, 0, 0, 1};
    /** Address written into the published descriptor. */
    std::array<unsigned char, kAddressOctets> advertisedAddress{127, 0, 0, 1};
    /** Address the Client's egress rewrite produces. Must reach the bound endpoint. */
    std::array<unsigned char, kAddressOctets> transportAddress{127, 0, 0, 1};
    /** Even UDP port, shared by all three addresses because the egress hook keeps the port. */
    std::uint16_t port{kDefaultPort};
    /** Entity indices held back from the client lease for server-authored entities. */
    std::uint16_t serverReserveCount{kDefaultServerReserve};
    /** Entity indices the join grants. The rest stay free for the client to request. */
    std::uint16_t clientJoinGrantCount{kDefaultClientJoinGrant};
};

/**
 * Checks one gameplay block for internal consistency.
 * @param settings Parsed or default gameplay settings.
 * @return True when the topology, port, and reserve can be bound and advertised together.
 */
[[nodiscard]] bool valid(const Settings& settings) noexcept;

/**
 * Reports the slots actually held back from the client lease.
 * A disabled channel authors no entities, so it reserves nothing and the client keeps the
 * whole slot space.
 * @param settings Active gameplay settings.
 * @return Configured reserve, or zero while the channel is off.
 */
[[nodiscard]] std::uint16_t effective_reserve(const Settings& settings) noexcept;

/**
 * Reports the entity slots one join grants.
 * A disabled channel reserves nothing, so the grant is bounded by the whole slot space instead.
 * @param settings Active gameplay settings.
 * @return Configured grant, capped at what the reserve leaves free.
 */
[[nodiscard]] std::size_t join_grant(const Settings& settings) noexcept;

} // namespace sunrise::core::settings::server::gameplay
