#pragma once

#include "internal.h"

namespace sunrise::server::gameplay::physics::host::detail {

[[nodiscard]] bool sync_actor_bodies(BubbleHost::Storage::WorldSlot& slot,
                                     const world::HostExecutionContext& context,
                                     std::span<const world::ActorSnapshot> actors,
                                     std::span<const world::CommittedEvent> events) noexcept;
[[nodiscard]] bool run_controller_navigation(BubbleHost::Storage::WorldSlot& slot,
                                             const world::HostExecutionContext& context,
                                             std::span<const world::ActorSnapshot> actors,
                                             std::span<const world::CommittedEvent> committedEvents,
                                             EventWriter& writer) noexcept;
[[nodiscard]] bool run_physics(BubbleHost::Storage::WorldSlot& slot,
                               const world::HostExecutionContext& context) noexcept;
[[nodiscard]] bool collect_pose_commits(BubbleHost::Storage::WorldSlot& slot,
                                        std::span<const world::ActorSnapshot> actors) noexcept;
[[nodiscard]] bool run_combat(BubbleHost::Storage::WorldSlot& slot,
                              const world::HostExecutionContext& context,
                              std::span<const world::ActorSnapshot> actors,
                              EventWriter& writer) noexcept;
[[nodiscard]] bool record_combat_poses(BubbleHost::Storage::WorldSlot& slot,
                                       const world::HostExecutionContext& context,
                                       std::span<const world::ActorSnapshot> actors) noexcept;
[[nodiscard]] bool run_triggers(BubbleHost::Storage::WorldSlot& slot,
                                const world::HostExecutionContext& context,
                                EventWriter& writer) noexcept;
[[nodiscard]] bool run_objective_credit(BubbleHost::Storage::WorldSlot& slot,
                                        const world::HostExecutionContext& context,
                                        EventWriter& writer) noexcept;
[[nodiscard]] bool run_timers(BubbleHost::Storage::WorldSlot& slot,
                              const world::HostExecutionContext& context,
                              EventWriter& writer) noexcept;

} // namespace sunrise::server::gameplay::physics::host::detail
