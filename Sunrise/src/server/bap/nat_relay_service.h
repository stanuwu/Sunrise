#pragma once

#include <cstdint>

#include "../../core/network_capacity.h"
#include "../../middleware/bap/nat_relay.h"

namespace sunrise::server::bap {
struct Session;
namespace nat_relay {
/** A notification retains the native publication that introduced each side of the pair. */
struct Publication {
    std::uint32_t connection{};
    std::uint64_t generation{};
    middleware::bap::nat_relay::SecureAddress address{};
};
/** One peer address this session asked to connect to, held until the next poll can pair it. */
struct PendingRequest {
    bool initiatePending{};
    std::uint64_t initiateGeneration{};
    middleware::bap::nat_relay::SecureAddress requestedPeer{};
};
/**
 * One relay pairing this session's slot owns; the peer holds its own mirror under this
 * session's id.
 */
struct PairState {
    std::uint32_t pairSession{};
    std::uint32_t peerRegistration{};
    Publication local{}, remote{};
    bool notificationPending{};
    bool failureRepushSpent{};
};
/** One relay slot per other player in the session; this account's own row needs none. */
inline constexpr std::size_t kPeerCapacity = core::network_capacity::kPlayers - 1;
/**
 * Per-session relay bookkeeping: requested peers awaiting a pairing attempt, and pairs
 * currently armed or valid.
 */
struct State {
    bool registered{};
    std::array<PendingRequest, kPeerCapacity> requests{};
    std::array<PairState, kPeerCapacity> pairs{};
};
/** All calls share the BAP session-table lock with request and deferred delivery. */
void register_client(Session& session) noexcept;
/**
 * Records one peer to pair with on the next poll. A live request for the same peer is reused;
 * a full request table drops it.
 */
void initiate(Session& session,
              const middleware::bap::nat_relay::InitiateRelayConnection& request) noexcept;
/**
 * Requests one repush of every currently valid pair's connectivity notice, at most once per
 * pair on each side.
 */
void connectivity_failure(Session& session) noexcept;
/** Retires every armed pair, releases the registry client, and clears this session's relay
    state. */
void release(Session& session) noexcept;
/** Resolves pending native introductions and retires pairs whose publications have departed. */
void service() noexcept;
/** One connection's poll services its own relay state, rather than rescanning every player. */
void service(Session& source) noexcept;
} // namespace nat_relay
} // namespace sunrise::server::bap
