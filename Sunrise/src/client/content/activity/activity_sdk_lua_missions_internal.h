#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "activity_sdk_lua_artifacts_internal.h"

namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal {

/** Direct owner indexes replace full global scans in every mission renderer. */
struct RenderIndex final {
    std::vector<std::vector<std::uint32_t>> squadsByScenario{};
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> scenesBySlot{};
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> tasksBySlot{};
    /** First cue row of each type-53 slot; a slot's cue rows are contiguous and cue-indexed. */
    std::unordered_map<std::uint32_t, std::uint32_t> dialogueCuesBySlot{};
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> dialogueBySlot{};
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> directivesBySlot{};
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> combatGroupsBySlot{};
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> abilitiesBySlot{};
    std::unordered_map<std::uint32_t, std::uint32_t> worldsByScenarioTag{};
};

/** Keeps the extracted name unchanged unless another row already owns that Lua key. */
[[nodiscard]] std::string append_unique_key(std::string& output,
                                            std::unordered_set<std::string>& used,
                                            std::string_view name,
                                            std::uint32_t fallback);

/** Emits one concrete mission module with no pack or resolver access. */
[[nodiscard]] bool render_mission(const Source& source,
                                  const RenderIndex& index,
                                  std::uint32_t scenarioIndex,
                                  const format::Scenario& scenario,
                                  SourceModule& module);

} // namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal
