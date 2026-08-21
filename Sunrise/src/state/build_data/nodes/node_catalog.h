#pragma once

#include <cstddef>
#include <span>

#include "definition.h"

namespace sunrise::state::build_data::nodes {

/** Clears every generated node definition. */
void clear() noexcept;

/**
 * Checks that the definitions are dense and in native index order.
 * @param definitions Candidate rows.
 * @return True when the rows fit storage, index n sits at position n, and no child count overflows.
 */
[[nodiscard]] bool valid(std::span<const Definition> definitions) noexcept;

/**
 * Replaces the generated node definitions in one step.
 * @param definitions Complete dense rows in native node order.
 * @return True when the rows pass the checks and fit fixed State storage.
 */
[[nodiscard]] bool replace(std::span<const Definition> definitions) noexcept;

/**
 * Runs one callable over every node that drives a value slot.
 * Held under the shared lock, so the callable must not re-enter this domain.
 * @param visit Receives each node owning at least one record and an addressable value slot.
 */
void for_each_driving(void* context,
                      void (*visit)(void* context, const Definition& definition)) noexcept;

/**
 * Copies every row in native node order.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold every row.
 */
[[nodiscard]] bool snapshot(std::span<Definition> output, std::size_t& count) noexcept;

/** @return Number of generated node definitions, read under the lock. */
[[nodiscard]] std::size_t count() noexcept;

/**
 * Sets the visibility gate of every lore book category over one account flag bank.
 * @param accountFlags Bank already filled from the authored policy.
 * @return Number of gates set.
 */
std::size_t apply_visibility(std::span<std::uint8_t> accountFlags) noexcept;

/**
 * Calls back for every node, under the shared lock.
 *
 * Copying the table out costs a hundred and fifty kilobytes a call, and both callers wanted only a
 * few fields of a few rows. The callback must not take the catalog lock again.
 * @param context Passed through untouched.
 * @param visit Called once per node in native order.
 */
void for_each(void* context, void (*visit)(void*, const Definition&) noexcept) noexcept;

/**
 * Sets the character scoped visibility gates of the lore book categories.
 * @param characterFlags Character bank already filled from the authored policy.
 * @return Number of gates set.
 */
std::size_t apply_character_visibility(std::span<std::byte> characterFlags) noexcept;

} // namespace sunrise::state::build_data::nodes
