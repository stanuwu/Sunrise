#pragma once

#include <cstddef>
#include <span>

#include "definition.h"

namespace sunrise::state::build_data::records {

/** Clears every generated record definition. */
void clear() noexcept;

/**
 * Checks that the definitions are dense and in native index order.
 * @param definitions Candidate rows.
 * @return True when the rows fit storage and index n sits at position n.
 */
[[nodiscard]] bool valid(std::span<const Definition> definitions) noexcept;

/**
 * Replaces the generated record definitions in one step.
 * @param definitions Complete dense rows in native index order.
 * @return True when the rows pass the checks and fit fixed State storage.
 */
[[nodiscard]] bool replace(std::span<const Definition> definitions) noexcept;

/**
 * Finds one record by the native row a claim names.
 * @param definitionIndex Native record row.
 * @param definition Receives the row only on success.
 * @return True when the domain is complete and the row exists.
 */
[[nodiscard]] bool find(std::uint16_t definitionIndex, Definition& definition) noexcept;

/**
 * Copies every row in native record order.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold every row.
 */
[[nodiscard]] bool snapshot(std::span<Definition> output, std::size_t& count) noexcept;

/** @return Number of generated record definitions, read under the lock. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::records
