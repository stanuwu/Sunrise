#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "definition.h"

namespace sunrise::state::build_data::socket_entry_lists {

/** Clears every generated socket-entry-list mapping. */
void clear() noexcept;

/**
 * Checks one complete dense socket-entry-list table.
 * @param definitions Candidate installed-build mappings.
 * @return True when every native index and ready mask is complete and in range.
 */
[[nodiscard]] bool valid(std::span<const Definition> definitions) noexcept;

/**
 * Replaces the generated socket-entry-list table in one step.
 * @param definitions Complete installed-build mappings in any input order.
 * @return True when every row passes validation and fits fixed State storage.
 */
[[nodiscard]] bool replace(std::span<const Definition> definitions) noexcept;

/**
 * Finds one socket-entry-list mapping by native definition index.
 * @param definitionIndex Nonnegative native definition index.
 * @param definition Receives the matching installed-build mapping.
 * @return True when the complete table contains the requested native row.
 */
[[nodiscard]] bool find(std::uint16_t definitionIndex, Definition& definition) noexcept;

/**
 * Copies every mapping in native definition-index order.
 * @param output Caller-owned fixed mapping storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold the complete table.
 */
[[nodiscard]] bool snapshot(std::span<Definition> output, std::size_t& count) noexcept;

/** @return The number of complete socket-entry-list mappings, read under the lock. */
[[nodiscard]] std::size_t count() noexcept;

/**
 * Checks the per-entry tables kept for lists that carry a super lane.
 * @param tables Candidate tables, each naming its own list index.
 * @return True when every row names a distinct list and fits fixed storage.
 */
[[nodiscard]] bool valid_entry_tables(std::span<const EntryTable> tables) noexcept;

/**
 * Replaces, in one step, the per-entry tables kept for lists that carry a super lane.
 * @param tables Candidate tables, each naming its own list index.
 * @return True when every row names a distinct list and fits fixed storage.
 */
[[nodiscard]] bool replace_entry_tables(std::span<const EntryTable> tables) noexcept;

/**
 * Finds one list's per-entry selection inputs.
 * @param definitionIndex Native socket-entry-list index.
 * @param table Receives the matching row.
 * @return True when a table is kept for that list.
 */
[[nodiscard]] bool find_entry_table(std::uint16_t definitionIndex, EntryTable& table) noexcept;

/**
 * Copies every kept entry table.
 * @param output Caller-owned fixed storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold every kept table.
 */
[[nodiscard]] bool snapshot_entry_tables(std::span<EntryTable> output, std::size_t& count) noexcept;

} // namespace sunrise::state::build_data::socket_entry_lists
