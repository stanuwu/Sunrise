#pragma once

#include "tick_runtime.h"

namespace sunrise::server::gameplay::physics::host::detail {

[[nodiscard]] bool execute_navigation_command(BubbleHost::Storage::WorldSlot& slot,
                                              const world::HostExecutionContext& context,
                                              const world::HostCommand& command,
                                              std::span<const world::ActorSnapshot> actors,
                                              EventWriter& writer) noexcept;
[[nodiscard]] bool execute_builtin_controller_command(BubbleHost::Storage::WorldSlot& slot,
                                                      const world::HostExecutionContext& context,
                                                      const world::HostCommand& command,
                                                      std::span<const world::ActorSnapshot> actors,
                                                      EventWriter& writer) noexcept;
[[nodiscard]] bool execute_combat_setup_command(BubbleHost::Storage::WorldSlot& slot,
                                                const world::HostExecutionContext& context,
                                                const world::HostCommand& command,
                                                std::span<const world::ActorSnapshot> actors,
                                                EventWriter& writer) noexcept;
[[nodiscard]] bool execute_trigger_command(BubbleHost::Storage::WorldSlot& slot,
                                           const world::HostExecutionContext& context,
                                           const world::HostCommand& command,
                                           EventWriter& writer) noexcept;
[[nodiscard]] bool execute_damage_command(BubbleHost::Storage::WorldSlot& slot,
                                          const world::HostExecutionContext& context,
                                          const world::HostCommand& command,
                                          std::span<const world::ActorSnapshot> actors,
                                          EventWriter& writer) noexcept;
[[nodiscard]] bool execute_ledger_command(BubbleHost::Storage::WorldSlot& slot,
                                          const world::HostExecutionContext& context,
                                          const world::HostCommand& command,
                                          EventWriter& writer) noexcept;
[[nodiscard]] bool execute_timer_command(BubbleHost::Storage::WorldSlot& slot,
                                         const world::HostExecutionContext& context,
                                         const world::HostCommand& command,
                                         EventWriter& writer) noexcept;

} // namespace sunrise::server::gameplay::physics::host::detail
