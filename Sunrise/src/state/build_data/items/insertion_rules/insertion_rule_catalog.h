#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "definition.h"

namespace sunrise::state::build_data::items::insertion_rules {

/** Discards every published rule row and forgets an unsupported settlement. */
void clear() noexcept;
/** True once the installed data has settled that no rule list can be published. */
[[nodiscard]] bool unsupported() noexcept;
/** Discards every rule row and records that the installed data supports none. */
void settle_unsupported() noexcept;
/** Checks rows in strictly ascending plug order with bounded, valid instructions. */
[[nodiscard]] bool valid(std::span<const Rule> rules) noexcept;
/** Publishes a validated rule list. */
[[nodiscard]] bool replace(std::span<const Rule> rules) noexcept;
/** Copies the rules one plug declares; false and an empty rule when it declares none. */
[[nodiscard]] bool find(std::uint16_t plugDefinitionIndex, Rule& rule) noexcept;
/** Copies every row; count is zero when the destination is too small. */
[[nodiscard]] bool snapshot(std::span<Rule> output, std::size_t& count) noexcept;
/** Number of published rows. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::items::insertion_rules
