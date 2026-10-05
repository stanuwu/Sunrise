#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "definition.h"

namespace sunrise::state::build_data::progressions {

/** Clears every generated progression definition and its step bank. */
void clear() noexcept;

/**
 * Checks that the definitions are dense, in native index order, and own the whole step bank.
 * @param definitions Candidate rows.
 * @param steps Candidate flat step bank in definition then rank order.
 * @return True when the rows fit storage, index n sits at position n, and the ranges are exact.
 */
[[nodiscard]] bool valid(std::span<const Definition> definitions,
                         std::span<const Step> steps) noexcept;

/**
 * Replaces the generated progression definitions and their step bank in one step.
 * @param definitions Complete dense rows in native index order.
 * @param steps Complete flat step bank in definition then rank order.
 * @return True when the rows pass the checks and fit fixed State storage.
 */
[[nodiscard]] bool replace(std::span<const Definition> definitions,
                           std::span<const Step> steps) noexcept;

/**
 * Reads one definition by its native index.
 * @param definitionIndex Native progression definition index.
 * @param definition Receives the definition on success.
 * @return False when the catalog does not hold the index.
 */
[[nodiscard]] bool find(std::uint16_t definitionIndex, Definition& definition) noexcept;

/**
 * Copies the rank steps one progression declares, in rank order.
 * @param definitionIndex Native progression definition index.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when the domain holds that definition and its whole range fits.
 */
[[nodiscard]] bool
steps(std::uint16_t definitionIndex, std::span<Step> output, std::size_t& count) noexcept;

/**
 * Copies the whole flat step bank.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold every row.
 */
[[nodiscard]] bool snapshot_steps(std::span<Step> output, std::size_t& count) noexcept;

/** @return The step bank row count, read under the lock. */
[[nodiscard]] std::size_t step_count() noexcept;

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
