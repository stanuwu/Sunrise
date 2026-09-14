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
struct PendingRequest {
    bool initiatePending{};
    std::uint64_t initiateGeneration{};
    middleware::bap::nat_relay::SecureAddress requestedPeer{};
};
struct PairState {
    std::uint32_t pairSession{};
    std::uint32_t peerRegistration{};
    Publication local{}, remote{};
    bool notificationPending{};
    bool failureRepushSpent{};
};
inline constexpr std::size_t kPeerCapacity = core::network_capacity::kPlayers - 1;
struct State {
    bool registered{};
    std::array<PendingRequest, kPeerCapacity> requests{};
    std::array<PairState, kPeerCapacity> pairs{};
};
/** All calls share the BAP session-table lock with request and deferred delivery. */
void register_client(Session& session) noexcept;
void initiate(Session& session,
              const middleware::bap::nat_relay::InitiateRelayConnection& request) noexcept;
void connectivity_failure(Session& session) noexcept;
void release(Session& session) noexcept;
/** Resolves pending native introductions and retires pairs whose publications have departed. */
void service() noexcept;
/** One connection's poll services its own relay state, rather than rescanning every player. */
void service(Session& source) noexcept;
} // namespace nat_relay
} // namespace sunrise::server::bap
