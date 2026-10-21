#pragma once

#include "internal.h"

namespace sunrise::server::bap {

/** Compares the authored selection, excluding each recipient's delivery bookkeeping. */
[[nodiscard]] bool same_mission_seed_plan(const ActivityMissionSeedPlan& left,
                                          const ActivityMissionSeedPlan& right) noexcept;

/** Prepares one recipient's selection without spending its publication receipt or arrival gate. */
[[nodiscard]] bool prepare_mission_seed_lease(const MissionSeedLease& current,
                                              const ActivityMissionSeedPlan& plan,
                                              std::uint64_t generation,
                                              std::int32_t heldRegion,
                                              MissionSeedLease& output) noexcept;

/** Matches a transport recipient to the complete shared activity identity. */
[[nodiscard]] bool mission_seed_recipient(const Session& session,
                                          const state::activity::SessionBinding& binding) noexcept;

/** Lets a new member inherit the script-selected state. Caller holds the BAP session lock. */
[[nodiscard]] bool inherit_mission_seed_locked(Session& session, std::int32_t heldRegion) noexcept;

/**
 * Reads the shared script selection before a joining link has a lease of its own.
 * Caller holds the BAP session lock. Conflicting current selections refuse the read; absent
 * selections succeed with selected=false. Neither case manufactures a publication receipt.
 */
[[nodiscard]] bool selected_mission_seed_locked(const state::activity::SessionBinding& binding,
                                                ActivityMissionSeedPlan& plan,
                                                bool& selected) noexcept;

/** True when the next roster owes a selection or complete-definition publication. */
[[nodiscard]] bool mission_seed_publication_pending(const MissionSeedLease& lease,
                                                    std::uint64_t generation) noexcept;

/** Commits only the generation and selection actually carried by the staged frame. */
void commit_mission_seed_publication(Session& session, const RosterPublication& staged) noexcept;

/** Requires this recipient's own receipt for the shared selection. */
[[nodiscard]] bool mission_seed_published(const Session& recipient,
                                          const ActivityMissionSeedPlan& plan) noexcept;

/** Validates and clears stale state on one already-selected ActivityClient session. */
[[nodiscard]] ActivityMissionSeedLeaseStatus mission_seed_session_status(
    Session& session, std::uint32_t scenarioRow, std::uint64_t expectedGeneration) noexcept;

/** Copies one validated session lease into the public read-only view. */
void read_mission_seed_lease(const Session& session,
                             std::size_t matchingLinks,
                             ActivityMissionSeedLeaseView& output) noexcept;

} // namespace sunrise::server::bap
