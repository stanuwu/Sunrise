#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "peer_transport.h"

namespace sunrise::server::gameplay::peer {

/** One out-of-band reply fits well inside a single unfragmented payload. */
inline constexpr std::size_t kReplyCapacity = 1024;

/** Guards the peer table and every installed callback. */
extern SRWLOCK g_lock;
extern std::array<state::gameplay::PeerLink, state::gameplay::kAssociationCapacity> g_peers;
/** Separates reused peer and channel storage. */
extern std::uint64_t g_peerGeneration;
extern middleware::gameplay::external::Lane0Codec g_lane0Codec;
extern void (*g_lane0Outcome)(const void*,
                              std::uint64_t,
                              middleware::gameplay::peer::AckOutcome) noexcept;
extern Lane0Transport g_lane0Transport;
extern middleware::gameplay::external::TypePayloadCodec g_entityCodec;
extern bool (*g_entityAccepted)(const void*,
                                std::uint64_t,
                                const middleware::gameplay::external::EntityBatch&) noexcept;
extern const void* g_entityAcceptedContext;
extern EntityTransport g_entityTransport;
/** Drops incomplete outer packets; callers hold the peer lock. */
void reset_packet_assemblies() noexcept;
/** Channel ids this host hands out. The peer refuses one that does not increase. */
extern std::uint32_t g_channelId;

/** One external outcome copied out before its callback runs. */
struct CompletedExternal {
    std::uint64_t groupSessionId{};
    std::uint64_t transmissionId{};
    middleware::gameplay::peer::AckOutcome outcome{
        middleware::gameplay::peer::AckOutcome::unresolved};
};

/** Every occupied external contribution one link is about to lose. */
using DisplacedExternals =
    std::array<CompletedExternal, state::gameplay::external::kExternalContributionCapacity>;

/**
 * Sends one payload over whichever transport the peer arrived on.
 * Records go first. The engine association answers only when no record association exists.
 * @param to Peer endpoint.
 * @param payload Payload bytes.
 * @return True when one of the two carried it.
 */
[[nodiscard]] bool send_transport(const state::gameplay::Endpoint& to,
                                  std::span<const std::byte> payload) noexcept;

/**
 * Copies out the contributions a link is about to drop, so their owner can be told.
 * @param peer Link being reset. Callers hold the lock.
 * @param displaced Receives one unresolved outcome per occupied contribution.
 * @return How many entries were filled.
 */
[[nodiscard]] std::size_t collect_displaced_locked(const state::gameplay::PeerLink& peer,
                                                   DisplacedExternals& displaced) noexcept;

/**
 * Reports one batch of external outcomes to whichever channel-0 sink is installed.
 * Runs outside the lock, because a callback takes it again.
 * @param completed Outcomes copied out under the lock.
 * @param count Entries in use.
 */
void notify_external_outcomes(const DisplacedExternals& completed, std::size_t count) noexcept;

/**
 * Tells the lane-0 transport that a group session lost its channel state.
 * @param sessions
 * Session ids copied out under the lock.
 * @param count Entries in use.
 */
void reset_transports(const std::uint64_t* sessions, std::size_t count) noexcept;

/** Tells the lane-0 transport that one group session lost its channel state. */
void reset_transports(std::uint64_t sessionId) noexcept;

/** Copies the exact source before its peer, view, or ActivityClient changes. */
[[nodiscard]] state::gameplay::entity_identity::Source
entity_source(const state::gameplay::PeerLink& peer) noexcept;

/** Retires identity evidence before a source changes; the caller holds the peer lock. */
void invalidate_entity_identity_locked(
    const state::gameplay::entity_identity::Source& source) noexcept;

/** Retires only the captured source, never a replacement sharing its group. */
void reset_entity_source(const state::gameplay::entity_identity::Source& source) noexcept;

/** @return Peer for one endpoint, or null. Callers already hold the lock. */
[[nodiscard]] state::gameplay::PeerLink*
find_locked(const state::gameplay::Endpoint& from) noexcept;

/** @return True when the link carries one group session. Callers hold the lock. */
[[nodiscard]] bool carries_locked(const state::gameplay::PeerLink& peer,
                                  std::uint64_t sessionId) noexcept;

/** @return Link carrying one group session, or null. Callers hold the lock. */
[[nodiscard]] state::gameplay::PeerLink* find_session_locked(std::uint64_t sessionId) noexcept;

/**
 * Resolves the session an out-of-band message at one endpoint belongs to.
 * @param from Peer endpoint.
 * @return The session id, or zero when it cannot be resolved.
 */
[[nodiscard]] std::uint64_t session_for_endpoint(const state::gameplay::Endpoint& from) noexcept;

/** @return A free peer slot, or null. Callers already hold the lock. */
[[nodiscard]] state::gameplay::PeerLink* allocate_locked() noexcept;

/**
 * Consumes one out-of-band message container.
 * @param from Peer endpoint.
 * @param payload Whole decrypted payload.
 * @param now Monotonic tick count.
 */
void consume_container(const state::gameplay::Endpoint& from,
                       std::span<const std::byte> payload,
                       std::uint64_t now) noexcept;

/**
 * Consumes one established packet.
 * @param from Peer endpoint.
 * @param payload Whole decrypted payload.
 * @param now Monotonic tick count.
 */
void consume_established(const state::gameplay::Endpoint& from,
                         std::span<const std::byte> payload,
                         std::uint64_t now) noexcept;
/** Restores an established participant channel from its own still-current native owner link. */
void bind_participant(const state::gameplay::Endpoint& from,
                      const state::gameplay::entity_identity::Source& ingress) noexcept;

} // namespace sunrise::server::gameplay::peer
