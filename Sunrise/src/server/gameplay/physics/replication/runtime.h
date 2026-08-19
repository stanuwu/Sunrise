#pragma once

#include <cstdint>

#include "definition.h"

namespace sunrise::server::gameplay::physics::replication {

/** Initializes one empty per-peer planner. */
[[nodiscard]] bool initialize_peer(state::gameplay::physics::PeerReplicaHandle replica,
                                   PeerState& peer) noexcept;

/** Releases every planner-owned ticket and actor reference. */
[[nodiscard]] bool reset_peer(PeerState& peer) noexcept;

/** Adds one relevant actor or publishes its newest immutable state. */
[[nodiscard]] bool publish_actor(PeerState& peer, const ActorSnapshot& actor) noexcept;

/** Marks one actor for a strict remove after its create may have landed. */
[[nodiscard]] bool request_remove(PeerState& peer,
                                  const state::gameplay::physics::Allocation& allocation) noexcept;

/** Selects the highest-priority actor contribution without mutating baselines. */
[[nodiscard]] PrepareResult prepare_next(PeerState& peer, Plan& plan) noexcept;

/** Attaches one prepared plan to an exact State ticket and packet generation. */
[[nodiscard]] bool commit_plan(PeerState& peer,
                               const Plan& plan,
                               std::uint64_t packetGeneration,
                               ContributionHandle& contribution) noexcept;

/** Settles one exact contribution without clearing newer desired state. */
[[nodiscard]] bool settle(PeerState& peer,
                          ContributionHandle contribution,
                          std::uint64_t packetGeneration,
                          Outcome outcome) noexcept;

/** Counts actors still owned by one peer planner. */
[[nodiscard]] std::size_t actor_count(const PeerState& peer) noexcept;

/** Counts external outcomes still waiting for a result. */
[[nodiscard]] std::size_t contribution_count(const PeerState& peer) noexcept;

} // namespace sunrise::server::gameplay::physics::replication
