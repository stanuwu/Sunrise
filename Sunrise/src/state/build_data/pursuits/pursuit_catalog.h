#pragma once

#include <span>

#include "definition.h"

namespace sunrise::state::build_data::pursuits {

/** Discards every contract and forgets an unsupported settlement. */
void clear() noexcept;
/** True once the contracts have been published, even when there are none. */
[[nodiscard]] bool ready() noexcept;
/** True once the installed data has settled that no contract can be read. */
[[nodiscard]] bool unsupported() noexcept;
/** Discards every contract and records that the installed data supports none. */
void settle_unsupported() noexcept;
/** @return A generation that changes after every publication, settlement or clear. */
[[nodiscard]] std::uint64_t revision() noexcept;
/**
 * Checks contract order and objective references.
 * @param objectiveCount Rows in the published objective table the contracts name.
 */
[[nodiscard]] bool valid(std::span<const Definition> rows, std::size_t objectiveCount) noexcept;
/** Publishes validated contracts under the catalog lock. */
[[nodiscard]] bool replace(std::span<const Definition> rows, std::size_t objectiveCount) noexcept;
/** Copies one item's contract; false when the item has none. */
[[nodiscard]] bool find(std::uint16_t itemIndex, Definition& output) noexcept;
/** Copies every contract; count is zero if the output is too small. */
[[nodiscard]] bool snapshot(std::span<Definition> output, std::size_t& count) noexcept;

} // namespace sunrise::state::build_data::pursuits
