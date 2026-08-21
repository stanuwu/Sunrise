#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::state::record_claims {

/**
 * Records claimed through Web Service opcode 1801, as account flag bank indices.
 *
 * The authored unlock policy is immutable for the life of the process, so a claim cannot write to
 * it. This holds the claims instead, and the account encoder lays them over the authored bank on
 * its way out. A claim is therefore visible to the client on the next Family-4 image.
 *
 * Claims are written to a file beside the build data cache, so they survive a restart. Settings
 * stay configuration: nothing here edits them.
 */

/**
 * Derives the claim file path and loads any claims already held.
 * A missing file is not a failure: it is an account that has claimed nothing yet.
 * @param module Loaded DLL, used to find the artifact directory.
 * @return True when the path resolves. Loading is best effort and reported separately.
 */
[[nodiscard]] bool initialize(void* module) noexcept;

/** Forgets every held claim, in memory only. The file is left alone. */
void clear() noexcept;

/**
 * Marks one account flag bank index claimed, adds its score, and writes the claim file.
 * A repeated claim of the same index is held once, scores once, and rewrites nothing.
 * @param flagIndex Mapping-table row whose object byte feeds the record's completion flag.
 * @param scoreValue Points the record is worth, counted only on the first claim.
 * @return True when the index is in range and the claim is now held.
 */
[[nodiscard]] bool claim(std::uint16_t flagIndex, std::uint16_t scoreValue) noexcept;

/**
 * Lays every held claim over one account flag bank.
 * @param accountFlags Bank already filled from the authored policy.
 * @return Number of bytes this changed, so a caller can tell a no-op from real work.
 */
std::size_t apply(std::span<std::uint8_t> accountFlags) noexcept;

/**
 * Writes each presentation node's claimed-child count into the value slot its bar reads.
 *
 * A node's progress bar is not derived by the client from its children: the node names a value slot
 * and shows whatever it holds. Counting here is what makes claiming a chapter move its book.
 * @param objectiveValues Account value bank, already filled from the authored policy.
 * @return Number of nodes whose slot was written.
 */
std::size_t apply_node_progress(std::span<std::int32_t> objectiveValues) noexcept;

/** @return True when this index is already held. */
[[nodiscard]] bool claimed(std::uint16_t flagIndex) noexcept;

/**
 * Writes each category's claimed-child count into the character value slot its bar reads.
 * One book counts in the character bank rather than the account one.
 * @param characterValues Character value bank, already filled from the authored policy.
 * @return Number of categories written.
 */
std::size_t apply_character_node_progress(std::span<std::int32_t> characterValues) noexcept;

/** @return Total score of every held claim. */
[[nodiscard]] std::uint32_t total_score() noexcept;

/** @return Number of distinct indices held. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::record_claims
