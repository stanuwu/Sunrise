#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "definition.h"

namespace sunrise::state::build_data::starter_sets {

/** Clears the published rows and marks the table unpublished. */
void clear() noexcept;

/**
 * Checks one whole table.
 * @param definitions One row per class, each class at most once.
 * @return False when a row names no playable class, repeats one, or overflows its items.
 */
[[nodiscard]] bool valid(std::span<const Definition> definitions) noexcept;

/**
 * Publishes the whole table in one step. An empty table is published too.
 * @param definitions One row per class, each class at most once.
 * @return False when the table fails its checks; the published table is then unchanged.
 */
[[nodiscard]] bool replace(std::span<const Definition> definitions) noexcept;

/** @return True once a table, empty or not, is published. */
[[nodiscard]] bool ready() noexcept;

/**
 * Finds the row of one class.
 * @param characterClass Class byte: 0 Titan, 1 Hunter, 2 Warlock.
 * @param definition Receives the row.
 * @return True when the published table holds that class.
 */
[[nodiscard]] bool find(std::uint8_t characterClass, Definition& definition) noexcept;

/**
 * Copies every published row in class order.
 * @param output Caller-owned row storage.
 * @param count Receives the copied row count, zero when output is too small.
 * @return False when output cannot hold every row.
 */
[[nodiscard]] bool snapshot(std::span<Definition> output, std::size_t& count) noexcept;

} // namespace sunrise::state::build_data::starter_sets
