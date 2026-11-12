#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "definition.h"

namespace sunrise::state::build_data::combat {

/** Discards every published row and forgets an unsupported settlement. */
void clear() noexcept;
/** True once the installed data has settled that no taxonomy can be read. */
[[nodiscard]] bool unsupported() noexcept;
/** Discards every row and records that the installed data supports no taxonomy. */
void settle_unsupported() noexcept;
/** Checks race rows in strictly ascending class order, none naming the empty hash. */
[[nodiscard]] bool valid(std::span<const EnemyRace> races) noexcept;
/** Checks group rows in strictly ascending group, then label, order. */
[[nodiscard]] bool valid(std::span<const LabelGroup> groups) noexcept;
/** Publishes both tables in one step after checking them. */
[[nodiscard]] bool replace(std::span<const EnemyRace> races,
                           std::span<const LabelGroup> groups) noexcept;
/** Copies the race of one actor class; false when the table does not name the class. */
[[nodiscard]] bool find_race(std::uint32_t classHash, std::uint8_t& race) noexcept;
/** @return True when the label sits in the named group. */
[[nodiscard]] bool in_group(std::uint32_t groupHash, std::uint32_t labelHash) noexcept;
/** Copies every race row; count is zero when the destination is too small. */
[[nodiscard]] bool snapshot(std::span<EnemyRace> output, std::size_t& count) noexcept;
/** Copies every group row; count is zero when the destination is too small. */
[[nodiscard]] bool snapshot(std::span<LabelGroup> output, std::size_t& count) noexcept;
/** Number of published race rows; the group table publishes with it. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::combat
