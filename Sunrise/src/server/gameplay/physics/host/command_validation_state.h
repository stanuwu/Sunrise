#pragma once

#include "internal.h"

namespace sunrise::server::gameplay::physics::host::detail {

[[nodiscard]] bool valid_combat_configuration(const BubbleHost::Storage::WorldSlot& slot,
                                              const world::HostCommand& command) noexcept;
[[nodiscard]] bool valid_trigger_effect(const BubbleHost::Storage::WorldSlot& slot,
                                        const world::HostCommand& command) noexcept;
[[nodiscard]] bool valid_objective_effect(const BubbleHost::Storage::WorldSlot& slot,
                                          const world::HostCommand& command) noexcept;
[[nodiscard]] bool valid_credit_effect(const BubbleHost::Storage::WorldSlot& slot,
                                       const world::HostCommand& command) noexcept;
[[nodiscard]] bool registered_attack_matches(const BubbleHost::Storage::WorldSlot& slot,
                                             const world::SubmitDamageCommand& damage,
                                             world::PolicyOwnerId owner) noexcept;
[[nodiscard]] bool
valid_builtin_controller_configuration(const BubbleHost::Storage::WorldSlot& slot,
                                       const world::ConfigureControllerCommand& command) noexcept;
[[nodiscard]] bool valid_timer_effect(const BubbleHost::Storage::WorldSlot& slot) noexcept;

} // namespace sunrise::server::gameplay::physics::host::detail
