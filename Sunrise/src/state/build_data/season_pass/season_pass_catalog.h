#pragma once

#include <span>

#include "definition.h"

namespace sunrise::state::build_data::season_pass {

/** Discards the installed pass, including its native claim indices. */
void clear() noexcept;
/** Checks row bounds; unavailable rows retain their native positions. */
[[nodiscard]] bool valid(std::span<const Reward> rewards) noexcept;
/** Publishes a validated pass without reordering its claim indices. */
[[nodiscard]] bool replace(std::span<const Reward> rewards) noexcept;
/** Copies one native claim row; clears the output when absent or unavailable. */
[[nodiscard]] bool find(std::uint16_t rewardIndex, Reward& reward) noexcept;
/** Copies the complete pass; count is zero when the destination is too small. */
[[nodiscard]] bool snapshot(std::span<Reward> output, std::size_t& count) noexcept;
/** Number of native positions, including unavailable rows. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::season_pass
