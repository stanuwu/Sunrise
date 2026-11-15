#pragma once

#include <cstdint>
#include <span>
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
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> temperamentsBySlot{};
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> channelsBySlot{};
    std::unordered_map<std::uint32_t, std::uint32_t> worldsByScenarioTag{};
};

/** Keeps the extracted name unchanged unless another row already owns that Lua key. */
[[nodiscard]] std::string append_unique_key(std::string& output,
                                            std::unordered_set<std::string>& used,
                                            std::string_view name,
                                            std::uint32_t fallback);

/** Appends one signed decimal literal. */
void append_int(std::string& output, std::int32_t value);

/** Appends the catalog-wide constant tables every mission module shares. */
void append_catalog_constants(const Source& source, std::string& output);

/**
 * Appends one scene's inputs and outputs, named where the trigger name is known.
 * An input is waited on and fired by no graph block; an output is fired and waited on by none.
 * @param scene The scene's Lua key.
 * @param gates The scene's gate rows, in graph order.
 * @param inputs Receives the scene's block of `mission.SceneInput`, when it has inputs.
 * @param outputs Receives the scene's block of `mission.SceneOutput`, when it has outputs.
 */
void append_scene_ports(std::string_view scene,
                        std::span<const format::AuthoredSceneEventKey> gates,
                        std::string& inputs,
                        std::string& outputs);

/** Emits one concrete mission module with no pack or resolver access. */
[[nodiscard]] bool render_mission(const Source& source,
                                  const RenderIndex& index,
                                  std::uint32_t scenarioIndex,
                                  const format::Scenario& scenario,
                                  SourceModule& module);

} // namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal
