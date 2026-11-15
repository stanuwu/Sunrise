#pragma once

#include "../../state/activity/membership/activity_membership_query.h"
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

/**
 * Holds a selection that reloads a recipient's held bubble until its message 33 releases it.
 * A pending release moves to a new selection in the same bubble and is dropped otherwise.
 * @param recipient Recipient link; its handoff changes.
 * @param plan Selection being made.
 * @param placement The recipient client's reported placement.
 * @return Ready when no release is owed or it was acknowledged, busy while it is pending.
 */
[[nodiscard]] ActivityMissionSeedLeaseStatus
prepare_bubble_handoff(Session& recipient,
                       const ActivityMissionSeedPlan& plan,
                       const state::activity::membership::ClientPlacement& placement) noexcept;

/** Matches a transport recipient to the complete shared activity identity. */
[[nodiscard]] bool mission_seed_recipient(const Session& session,
                                          const state::activity::SessionBinding& binding) noexcept;

/** Matches the public-target link the recipient's own client holds beside it. */
[[nodiscard]] bool mission_seed_public_link(const Session& link, const Session& recipient) noexcept;

/** @return True when this link hosts the bubble the plan's state belongs to. */
[[nodiscard]] bool mission_seed_plan_hosted(const Session& session,
                                            const ActivityMissionSeedPlan& plan) noexcept;

/**
 * Reads whether every current recipient has the plan's bubble published: by its own receipt
 * when it hosts the bubble, else by its client's public link that hosts it, which publishes the
 * plan for it. Caller holds the BAP session lock.
 * @param everyClient True holds for a recipient whose client has no link in the bubble: the
 * initial-state pass waits for the clients to arrive. A client that stands in an alternate state
 * of the bubble on its private link alone has arrived. False lets it through, for script output
 * that nothing can carry into a bubble no client hosts; with no hosting link at all it is ready.
 */
[[nodiscard]] bool
mission_seed_hosted_publication_locked(const state::activity::SessionBinding& binding,
                                       const ActivityMissionSeedPlan& plan,
                                       bool everyClient) noexcept;

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

/**
 * Reads whether a public link owes its client a roster for its owner's mission: a new owner, a
 * newer owner estate, an unpublished owner selection or a pending owner body in a bubble it
 * publishes for that owner. Caller holds the BAP session lock.
 */
[[nodiscard]] bool owner_publication_due_locked(const Session& link) noexcept;

/** Commits the owner selection a public link's staged frame carried. */
void commit_owner_seed_publication(Session& link, const RosterPublication& staged) noexcept;

/**
 * @return The bit of the bubble a public link's host row was advertised for, numbered in the
 * world of the private link that advertised it; zero before the link has one.
 */
[[nodiscard]] std::uint64_t advertised_bubble(const Session& link) noexcept;

/**
 * Reads the bubbles a public link publishes for its owner's mission: the one it was advertised for,
 * when the owner's world authors that bubble public. The public link's own free-roam world says
 * nothing about the mission's bubbles.
 */
[[nodiscard]] std::uint64_t owner_public_bubbles(const Session& link,
                                                 const Session& owner) noexcept;

/**
 * Finds the private link a public link stands beside: the same account's joined private link.
 * Caller holds the BAP session lock.
 * @return Null when there is none or more than one.
 */
[[nodiscard]] Session* public_mission_owner_locked(const Session& link) noexcept;

/**
 * Reads whether a group is in every state of the owner's scenario. The private link registers
 * such a group in its top-level list, so its public link never publishes it.
 */
[[nodiscard]] bool owner_scenario_wide(const Session& owner,
                                       std::uint32_t objectTag,
                                       std::uint32_t registryKey) noexcept;

/**
 * Reads whether a state-local target of the owner's mission lies in a state the owner's world
 * authors public, so the public link beside it publishes the target. A group in every state of
 * the scenario stays in the private link's top-level list.
 */
[[nodiscard]] bool publishes_on_public(const Session& owner,
                                       const activity::host::ScriptableTarget& target) noexcept;

/**
 * Finds the public link of the owner's client that hosts the target's bubble.
 * Caller holds the BAP session lock.
 */
[[nodiscard]] const Session*
public_publisher_locked(const Session& owner,
                        const activity::host::ScriptableTarget& target) noexcept;

/**
 * Requires a hosting link's own receipt for whichever state it adopted in the region. A public
 * link's receipt is the one it holds for its owner's selection.
 */
[[nodiscard]] bool mission_seed_region_published(const Session& link,
                                                 std::uint32_t effectiveRegion) noexcept;

/** Requires this recipient's own receipt for the shared selection. */
[[nodiscard]] bool mission_seed_published(const Session& recipient,
                                          const ActivityMissionSeedPlan& plan) noexcept;

/** Validates and clears stale state on one already-selected ActivityClient session. */
[[nodiscard]] ActivityMissionSeedLeaseStatus mission_seed_session_status(
    Session& session, std::uint32_t scenarioRow, std::uint64_t expectedGeneration) noexcept;

/**
 * Copies one validated session lease into the public read-only view. Caller holds the BAP
 * session lock. The view is the link's own receipt, earned by its state-local roster whether
 * or not it hosts the plan's bubble; the bubble's entity definitions are
 * mission_seed_hosted_publication_locked's question.
 */
void read_mission_seed_lease(const Session& session,
                             std::size_t matchingLinks,
                             ActivityMissionSeedLeaseView& output) noexcept;

} // namespace sunrise::server::bap
