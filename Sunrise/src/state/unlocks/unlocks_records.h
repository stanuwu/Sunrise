#pragma once

#include <cstdint>

namespace sunrise::state::unlocks::records {

/** Result of advancing a single-objective record. */
enum class ObjectiveAdvance : std::uint8_t {
    unavailable,
    alreadyHeld,
    advanced,
    completed,
};

/** @return True when the advance moved the record's progress. */
[[nodiscard]] constexpr bool progress_moved(ObjectiveAdvance advance) noexcept {
    return advance == ObjectiveAdvance::advanced || advance == ObjectiveAdvance::completed;
}

/** Why one exact lore chapter grant did not change the banks. */
enum class GrantOutcome : std::uint8_t {
    granted,
    progressed,
    refused,
    recordNotFound,
    noFlag,
    alreadyHeld,
    notAChapter,
};

/**
 * Replaces the authored lore values with the ones the seeded claims imply.
 * Runs once, after the record and node catalogs are published.
 */
void seed() noexcept;

/** Re-derives every bar and gate after a record or node catalog is replaced. */
void republish() noexcept;

/** @param flagIndex Account flag bank row. @return True when that record is claimed. */
[[nodiscard]] bool claimed(std::uint16_t flagIndex) noexcept;

/** @param flagIndex Account flag bank row. @return True when complete but unclaimed. */
[[nodiscard]] bool claimable(std::uint16_t flagIndex) noexcept;

/**
 * Claims one record: sets its flag, adds its score, and re-derives the bars it feeds.
 * @param recordIndex Native record row an opcode-1801 claim names.
 * @return False when the row has no addressable flag or is already claimed.
 */
[[nodiscard]] bool claim(std::uint16_t recordIndex) noexcept;

/**
 * Undoes one claim so a refused commit cannot leave the record held.
 * The record stays complete, which is what it was before the claim.
 * @param recordIndex Native record row passed to claim().
 */
void revoke(std::uint16_t recordIndex) noexcept;

/**
 * Redeems the next earned step of a record that scores per step. A record with a completion flag
 * sets it once its last step is redeemed.
 * @param recordIndex Native record row.
 * @param definitionHash Authored hash the claim named, checked against the row.
 * @return False when the row is not an interval record, every step is redeemed, or the next step
 *         is not earned.
 */
[[nodiscard]] bool claim_interval(std::uint16_t recordIndex, std::uint32_t definitionHash) noexcept;

/**
 * @param recordIndex Native record row.
 * @return True when the record has a completion flag and its next interval claim redeems its
 *         last step, which sets that flag.
 */
[[nodiscard]] bool interval_claim_completes(std::uint16_t recordIndex) noexcept;

/**
 * Adds one to the progress every step of an interval record reads.
 * @param definitionHash Authored record definition hash.
 * @return Completed when a step becomes earned; already held at the last step or once claimed.
 */
[[nodiscard]] ObjectiveAdvance advance_interval(std::uint32_t definitionHash) noexcept;

/**
 * Marks one lore chapter collected.
 * @param recordIndex Native record row of the chapter.
 * @return What the grant changed, or why it changed nothing.
 */
[[nodiscard]] GrantOutcome grant_chapter(std::uint16_t recordIndex) noexcept;

/**
 * Advances the single-objective record that owns one completion flag.
 * @param flagIndex Account flag bank row.
 * @return What the advance changed, or why it changed nothing.
 */
[[nodiscard]] ObjectiveAdvance advance_objective(std::uint16_t flagIndex) noexcept;

/** @return Triumph score published in the account value bank. */
[[nodiscard]] std::uint32_t score() noexcept;

} // namespace sunrise::state::unlocks::records
