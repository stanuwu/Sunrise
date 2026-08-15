#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "definition.h"

namespace sunrise::state::build_data::hash_names {

/** Clears every resolved bubble name. */
void clear() noexcept;

/**
 * Checks one complete table in ascending hash order.
 * @param names Candidate rows.
 * @return True when every row is valid and the hashes rise strictly.
 */
[[nodiscard]] bool valid(std::span<const Name> names) noexcept;

/**
 * Replaces the resolved bubble names in one step.
 * @param names Complete rows in ascending hash order, or an empty complete table.
 * @return True when the rows pass the checks and fit fixed State storage.
 */
[[nodiscard]] bool replace(std::span<const Name> names) noexcept;

/**
 * Finds one bubble name by its hash.
 * @param hash Bubble name hash from a destination layout.
 * @param name Receives the matching row.
 * @return True when the table carries that exact hash.
 */
[[nodiscard]] bool find(std::uint32_t hash, Name& name) noexcept;

/**
 * Copies every row in ascending hash order.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold every row.
 */
[[nodiscard]] bool snapshot(std::span<Name> output, std::size_t& count) noexcept;

/** @return Number of resolved names, read under the lock. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::hash_names
