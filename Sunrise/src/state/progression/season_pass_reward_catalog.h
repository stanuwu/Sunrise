#pragma once

#include <cstdint>

#include "../build_data/season_pass/definition.h"

namespace sunrise::state::progression::season_pass {

/** Account progression used by the installed Season of Arrivals pass. */
inline constexpr std::uint16_t kProgressionDefinitionIndex = 40;
/** Repeating HUD bar paired with the Arrivals progression. */
inline constexpr std::uint16_t kHudProgressionDefinitionIndex = 41;

/** Acquisition flag of a direct perk reward, or build_data::rewards::kAbsent for an item or
 * package. */
[[nodiscard]] std::uint16_t progress_flag(const build_data::season_pass::Reward& reward) noexcept;

} // namespace sunrise::state::progression::season_pass
