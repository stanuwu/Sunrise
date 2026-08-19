#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../middleware/encoding/bit_reader.h"
#include "../../../state/gameplay/definition.h"

namespace sunrise::server::gameplay::group {

/**
 * Consumes one group-session message.
 * False leaves the reader part way through the body, so the caller must stop.
 * @param from Peer endpoint in host order.
 * @param sessionId Fallback for a message that names no session, or zero when the link carries
 *                  several. Every session-scoped message names its own and uses that instead.
 * @param id Registry message id.
 * @param reader Reader positioned at the message body.
 * @param now Monotonic tick count in milliseconds.
 * @return True when the body was decoded and the container may continue.
 */
[[nodiscard]] bool consume(const state::gameplay::Endpoint& from,
                           std::uint64_t sessionId,
                           std::uint8_t id,
                           middleware::encoding::bits::Reader& reader,
                           std::uint64_t now) noexcept;

/**
 * Publishes the membership snapshot that completes one peer's join.
 * It names the host and the peer, because the peer looks itself up by address. The peer accepts
 * it only while the state replica behind its hash is byte exact.
 * @param peer Endpoint of the admitted peer, in host order.
 * @param peerJoinId Join id the peer's join request carried. Its entry has to echo it.
 * @param sessionId Group-session id the peer named, which its parameter updates have to echo.
 * @return True when the snapshot was queued on the peer's reliable channel.
 */
[[nodiscard]] bool publish_membership(const state::gameplay::Endpoint& peer,
                                      std::uint64_t peerJoinId,
                                      std::uint64_t sessionId) noexcept;

/** One admitted peer's group session and how far its join has got. */
struct AdmittedRow {
    std::uint64_t sessionId{};
    state::gameplay::Endpoint endpoint{};
    /** Set once the peer reported its join finished. */
    bool joinComplete{};
    /** Set once the `activity-host` parameter is on the peer's reliable channel. */
    bool activityHostPublished{};
    /** Set once the peer has asked this host to add a player. */
    bool hasPlayer{};
    /** Set once a snapshot naming the peer's player is on that channel. */
    bool playerPublished{};
    /** Join id the peer chose for itself. It is the only member identity this table carries. */
    std::uint64_t joinId{};
};

/** Copies every admitted group-session record. @param count Receives the copied row count. */
void snapshot_admitted(std::span<AdmittedRow> output, std::size_t& count) noexcept;

/**
 * Retries any publish the reliable queue refused, on a timer.
 * The queue is fullest while the join promotion and the `activity-host` parameter are owed, so
 * both track what was queued and not what was asked for.
 * @param now Monotonic tick count in milliseconds.
 */
void service(std::uint64_t now) noexcept;

/**
 * Publishes the parameter update a joining peer needs before it will finish its join.
 * The peer's own tick refuses to complete until it has applied one, whatever the update names.
 * @param sessionId Group-session id the peer named in its join request.
 * @return True when the update was queued on the peer's reliable channel.
 */
[[nodiscard]] bool publish_join_parameters(std::uint64_t sessionId) noexcept;

/**
 * Reports whether replication may produce entity output for one peer.
 * View establishment is a hard gate: a mismatched signature means no entity output at all.
 * @param sessionId Group session the link carries.
 * @return True only once a compatible view is bound.
 */
[[nodiscard]] bool view_accepted(std::uint64_t sessionId) noexcept;

/**
 * Frees every admitted record at one endpoint.
 * Only an association timeout calls this. A connect-closed must not: the client rebuilds its
 * channel within 50 ms and keeps every group session it held.
 * @param endpoint Peer endpoint in host order, whose links the caller is already dropping.
 */
void release_endpoint(const state::gameplay::Endpoint& endpoint) noexcept;

/** Clears every group-session record. */
void reset() noexcept;

} // namespace sunrise::server::gameplay::group
