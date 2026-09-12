#pragma once

#include <array>
#include <span>
#include <string_view>

#include "../../middleware/content/packages/tables/component_container_reader.h"
#include "../../middleware/content/packages/tables/region_reader.h"
#include "../../state/activity/forced/definition.h"
#include "../../state/build_data/activities/activity_catalog.h"
#include "../../state/build_data/runtime.h"

namespace sunrise::client::activity::mission_launch {
namespace forced = state::activity::forced;
namespace layouts = state::build_data::scenarios;
namespace spawns = state::build_data::spawn_sets;
namespace tables = middleware::content::packages::tables;

enum class ManualError : std::uint8_t { none, activity, bubble, slice, spawn };

[[nodiscard]] inline std::string_view
destination_name(const forced::ForcedDestination& value) noexcept {
    return value.packageNameLength <= value.packageName.size()
               ? std::string_view(value.packageName.data(), value.packageNameLength)
               : std::string_view{};
}

[[nodiscard]] inline bool
manual_transport_valid(std::uint16_t index,
                       const forced::ForcedDestination& value,
                       std::span<const state::build_data::activities::Definition> rows) noexcept {
    return index < rows.size() && !rows[index].name().empty()
           && rows[index].name() == destination_name(value) && value.hasActivityIndex
           && value.activityIndex == index;
}

/** Exact destination/bubble/package membership; map-wide candidates are not proven arrival points.
 */
[[nodiscard]] inline bool spawn_supported(const layouts::Definition& layout,
                                          std::uint8_t bubble,
                                          const spawns::NameHash& row) noexcept {
    if (bubble >= layout.bubbleCount || bubble >= layout.bubbleMapIndices.size()
        || row.pointCount == 0 || row.activityPackageOverflow != 0
        || row.activityPackageCount > row.activityPackages.size()
        || layout.packageCount > layout.packages.size()
        || !tables::bubble_in_mask(row.bubbleMask, layout.bubbleMapIndices[bubble])) {
        return false;
    }
    if (row.inMapPackage != 0) {
        return true;
    }
    for (std::size_t i = 0; i < row.activityPackageCount; ++i) {
        for (std::size_t j = 0; j < layout.packageCount; ++j) {
            if (row.activityPackages[i] == layout.packages[j]) {
                return true;
            }
        }
    }
    return false;
}

/** Pure launch argument check, shared by the draft UI and the game-frame request owner. */
[[nodiscard]] inline ManualError
validate_manual(const forced::ForcedDestination& value,
                const layouts::Definition& layout,
                std::span<const spawns::NameHash> spawnRows) noexcept {
    const auto name = destination_name(value);
    if (!value.enabled || name.empty() || layout.nameLength > layout.name.size()
        || name != std::string_view(layout.name.data(), layout.nameLength) || layout.truncated != 0
        || layout.bubbleCount > layout.bubbleStates.size()) {
        return ManualError::activity;
    }
    if (!value.hasBubble || value.bubble >= layout.bubbleCount) {
        return ManualError::bubble;
    }
    const auto states = layout.bubbleStateCounts[value.bubble];
    const auto base = tables::region_index(value.bubble);
    if (!value.hasSliceSet || states == 0 || states > tables::kSliceSetIndexFactor
        || value.sliceSet < base || value.sliceSet >= base + states) {
        return ManualError::slice;
    }
    if (value.hasSpawnSetHash) {
        if (value.spawnSetHash == 0 || value.spawnSetHash == forced::kAbsentSpawnSetHash) {
            return ManualError::spawn;
        }
        for (const auto& row : spawnRows) {
            if (row.value == value.spawnSetHash && spawn_supported(layout, value.bubble, row)) {
                return ManualError::none;
            }
        }
        return ManualError::spawn;
    }
    return ManualError::none;
}

/** Caller-owned storage keeps independent render/game-thread reads outside the UI arena and stack.
 */
struct ManualScratch {
    layouts::Definition layout{};
    std::array<spawns::NameHash, spawns::kNameHashCapacity> spawns{};
    std::size_t spawnCount{};
};

[[nodiscard]] inline ManualError validate_manual(const forced::ForcedDestination& value,
                                                 ManualScratch& scratch) noexcept {
    scratch.layout = {};
    scratch.spawnCount = 0;
    if (!state::build_data::find_scenario_layout(destination_name(value), scratch.layout)) {
        return ManualError::activity;
    }
    if (value.hasSpawnSetHash
        && scratch.layout.spawnStemLength <= scratch.layout.spawnStem.size()) {
        const std::string_view stem(scratch.layout.spawnStem.data(),
                                    scratch.layout.spawnStemLength);
        if (!state::build_data::find_spawn_sets(stem, scratch.spawns, scratch.spawnCount)
            || scratch.spawnCount > scratch.spawns.size()) {
            scratch.spawnCount = 0;
        }
    }
    return validate_manual(
        value, scratch.layout, std::span(scratch.spawns).first(scratch.spawnCount));
}

[[nodiscard]] inline const char* manual_error(ManualError error) noexcept {
    switch (error) {
    case ManualError::none:
        return "Ready to apply the selected arrival override.";
    case ManualError::activity:
        return "Select an available activity layout.";
    case ManualError::bubble:
        return "Select a bubble from this activity.";
    case ManualError::slice:
        return "Select an authored slice from the selected bubble.";
    case ManualError::spawn:
        return "This spawn is not proven to load in the selected activity and bubble. Choose "
               "another or let the client pick.";
    }
    return "Manual selection unavailable.";
}
} // namespace sunrise::client::activity::mission_launch
