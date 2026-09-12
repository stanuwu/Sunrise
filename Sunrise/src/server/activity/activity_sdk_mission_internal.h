#pragma once

#include <cstdint>
#include <span>

#include "../../state/build_data/runtime.h"
#include "../bap/runtime.h"
#include "activity_sdk_mission_runtime.h"
#include "host_runtime.h"

namespace sunrise::server::activity::activity_sdk_mission::detail {

/** Exact private route retained while one authored-scene request is staged. */
struct PreparedScene final {
    host::ScriptableTarget target{};
    state::build_data::scenarios::RosterGroup rosterGroup{};
    middleware::bap::activity_message::sensor_auth_update::AuthoredSceneDependencies
        sceneDependencies{};
    std::uint64_t activityClientGeneration{};
    std::uint32_t scenarioRow{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t stateRow{state::activity_sdk::format::kAbsentIndex};
    std::int32_t effectiveRegion{-1};
};

/** Maps exact SDK binding validation to this facade's stable refusal surface. */
[[nodiscard]] Status binding_status(const state::activity_sdk::BoundView& view,
                                    server::bap::ActivityLinkView& link) noexcept;

/** Maps the shared binding result to the authored-scene refusal surface. */
[[nodiscard]] SceneStatus scene_binding_status(const state::activity_sdk::BoundView& view,
                                               server::bap::ActivityLinkView& link) noexcept;

/** Maps the transport lease's closed set of connection and refusal outcomes. */
[[nodiscard]] Status lease_status(server::bap::ActivityMissionSeedLeaseStatus status) noexcept;

/** @return True when two plans name the same immutable generated rows and counts. */
[[nodiscard]] bool same_plan(const server::bap::ActivityMissionSeedPlan& left,
                             const server::bap::ActivityMissionSeedPlan& right) noexcept;

/**
 * Materializes into static lock-owned storage so UI stack size stays bounded.
 * @param omissions Objects the caller leaves out of the seed.
 * @param output Cleared, then filled only when the materializer reports ready.
 */
[[nodiscard]] Status
materialize_plan(const state::activity_sdk::BoundView& view,
                 std::int32_t effectiveRegion,
                 std::span<const state::activity_sdk::MissionSeedOmission> omissions,
                 server::bap::ActivityMissionSeedPlan& output) noexcept;

/** Reads the transport lease after the binding has been revalidated. */
[[nodiscard]] Status read_lease(const state::activity_sdk::BoundView& view,
                                const server::bap::ActivityLinkView& link,
                                server::bap::ActivityMissionSeedLeaseView& output) noexcept;

/** Resolves one exact generated scene without changing transport state. */
[[nodiscard]] SceneStatus prepare_scene(const state::activity_sdk::BoundView& view,
                                        std::uint32_t occurrenceRow,
                                        std::uint32_t slotRow,
                                        PreparedScene& output) noexcept;

/**
 * Resolves one exact SDK-bounded type-53 cue without changing transport state.
 * @param authoredCueCount Receives the authored cue count the list declares.
 */
[[nodiscard]] SceneStatus prepare_dialogue(const state::activity_sdk::BoundView& view,
                                           std::uint32_t occurrenceRow,
                                           std::uint32_t slotRow,
                                           std::uint16_t cueIndex,
                                           PreparedScene& output,
                                           std::uint16_t& authoredCueCount) noexcept;

/**
 * Resolves one exact fixed-schema behavior slot without changing transport state.
 * @param requireTaskTarget Also demands the slot carry an authored task target.
 */
[[nodiscard]] SceneStatus prepare_typed_behavior(const state::activity_sdk::BoundView& view,
                                                 std::uint32_t occurrenceRow,
                                                 std::uint32_t slotRow,
                                                 std::uint32_t expectedSlotType,
                                                 std::uint32_t expectedComponentClass,
                                                 std::uint32_t expectedAuthSchema,
                                                 bool requireTaskTarget,
                                                 PreparedScene& output) noexcept;

/** Resolves one exact SDK-linked type-38 task without changing transport state. */
[[nodiscard]] SceneStatus prepare_task(const state::activity_sdk::BoundView& view,
                                       std::uint32_t occurrenceRow,
                                       std::uint32_t slotRow,
                                       PreparedScene& output) noexcept;

/** Resolves one exact type-68 HUD state without changing transport state. */
[[nodiscard]] SceneStatus prepare_directive(const state::activity_sdk::BoundView& view,
                                            std::uint32_t occurrenceRow,
                                            std::uint32_t slotRow,
                                            std::uint32_t nameHash,
                                            std::int32_t elementIndex,
                                            PreparedScene& output) noexcept;

/** Resolves one exact type-3 objective sensor without changing transport state. */
[[nodiscard]] SceneStatus prepare_objective(const state::activity_sdk::BoundView& view,
                                            std::uint32_t occurrenceRow,
                                            std::uint32_t slotRow,
                                            PreparedScene& output) noexcept;

} // namespace sunrise::server::activity::activity_sdk_mission::detail
