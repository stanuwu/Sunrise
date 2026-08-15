#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "definition.h"

namespace sunrise::state::build_data::spawn_sets {

/** Clears every extracted stem and name-hash row. */
void clear() noexcept;

/**
 * Checks one complete spawn-set catalog in canonical stem and hash order.
 * Both arrays are checked together. A stem names its hashes by range, so a bank short by one row
 * silently repoints every stem above it.
 * @param stems Candidate stem rows.
 * @param nameHashes Candidate flat name-hash bank.
 * @return True when every range, count, name, and ordering rule holds.
 */
[[nodiscard]] bool valid(std::span<const Stem> stems,
                         std::span<const NameHash> nameHashes) noexcept;

/**
 * Replaces the complete spawn-set catalog in one step.
 * @param stems Complete stem rows in ascending name order.
 * @param nameHashes Complete flat bank in stem then hash order.
 * @return True when the catalog passes validation and fits fixed State storage.
 */
[[nodiscard]] bool replace(std::span<const Stem> stems,
                           std::span<const NameHash> nameHashes) noexcept;

/**
 * Finds one stem by its normalized name.
 * @param name Normalized map-package stem.
 * @param stem Receives the matching row.
 * @return True when the exact stem is present.
 */
[[nodiscard]] bool find(std::string_view name, Stem& stem) noexcept;

/**
 * Finds one spawn-name hash inside one stem.
 * @param stem Normalized map-package stem.
 * @param value Spawn-set name hash.
 * @param nameHash Receives the matching row.
 * @return True when the stem carries that exact hash.
 */
[[nodiscard]] bool
find_hash(std::string_view stem, std::uint32_t value, NameHash& nameHash) noexcept;

/**
 * Copies the name-hash rows one stem owns, in ascending hash order.
 * @param stem Stem whose range is copied.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold the whole range.
 */
[[nodiscard]] bool
stem_hashes(const Stem& stem, std::span<NameHash> output, std::size_t& count) noexcept;

/**
 * Copies every stem row in ascending name order.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold every row.
 */
[[nodiscard]] bool snapshot(std::span<Stem> output, std::size_t& count) noexcept;

/**
 * Copies the whole flat name-hash bank.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold every row.
 */
[[nodiscard]] bool snapshot_hashes(std::span<NameHash> output, std::size_t& count) noexcept;

/** @return The stem row count, read under the lock. */
[[nodiscard]] std::size_t count() noexcept;

/** @return The name-hash row count, read under the lock. */
[[nodiscard]] std::size_t hash_count() noexcept;

} // namespace sunrise::state::build_data::spawn_sets
