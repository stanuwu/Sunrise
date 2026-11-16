#pragma once

#include <algorithm>
#include <utility>

#include "mission_script_sdk_bridge.h"

namespace sunrise::server::activity::mission::sdk_bridge::combat_objective {

/** Accepts only an extracted task group on the current SDK objective slot. */
[[nodiscard]] inline bool resolve(const void* context,
                                  std::uint32_t slotRow,
                                  std::uint32_t groupIndex,
                                  lua_vm::CombatObjectiveGroupDefinition& output) noexcept {
    output = {};
    namespace sdk = state::activity_sdk;
    const auto* const view = static_cast<const sdk::BoundView*>(context);
    if (view == nullptr || view->catalog == nullptr || sdk::bound_activity(*view) == nullptr
        || sdk::bound_scenario(*view) == nullptr) {
        return false;
    }
    const auto groups = view->catalog->combat_objective_groups();
    const auto key = std::pair{slotRow, groupIndex};
    const auto found = std::lower_bound(
        groups.begin(), groups.end(), key, [](const auto& row, const auto& selected) {
            return std::pair{row.slotIndex, row.groupIndex} < selected;
        });
    const auto slots = view->catalog->slots();
    const auto objects = view->catalog->objects();
    if (found == groups.end() || found->slotIndex != slotRow || found->groupIndex != groupIndex
        || slotRow >= slots.size() || slots[slotRow].objectIndex >= objects.size()) {
        return false;
    }
    output = {slotRow,
              groupIndex,
              objects[slots[slotRow].objectIndex].objectKey,
              static_cast<std::uint16_t>(slots[slotRow].slotIndex),
              found->taskCount,
              found->tasks};
    return true;
}

} // namespace sunrise::server::activity::mission::sdk_bridge::combat_objective
