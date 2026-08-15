#pragma once

#include <cstddef>
#include <span>

#include "definition.h"

namespace sunrise::state::build_data::progressions {

/** Clears every generated progression definition. */
void clear() noexcept;

/**
 * Checks that the definitions are dense and in native index order.
 * @param definitions Candidate rows.
 * @return True when the rows fit storage and index n sits at position n.
 */
[[nodiscard]] bool valid(std::span<const Definition> definitions) noexcept;

/**
 * Replaces the generated progression definitions in one step.
 * @param definitions Complete dense rows in native index order.
 * @return True when the rows pass the checks and fit fixed State storage.
 */
[[nodiscard]] bool replace(std::span<const Definition> definitions) noexcept;

/**
 * Lists the definition index each slot of one scope's record array carries.
 * Slot order is native definition order among the definitions sharing that scope.
 * @param scope Replicated object owning the array.
 * @param output Slot storage, one entry per slot the array holds.
 * @param count Receives the number of keyed slots.
 * @return True when the domain is complete and every keyed slot fits.
 */
[[nodiscard]] bool slots(Scope scope, std::span<std::uint16_t> output, std::size_t& count) noexcept;

/**
 * Copies every row in native definition order.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold every row.
 */
[[nodiscard]] bool snapshot(std::span<Definition> output, std::size_t& count) noexcept;

/** @return Number of generated progression definitions, read under the lock. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::progressions
