#pragma once

#include <cstdint>

namespace sunrise::middleware::gameplay::dtls {

/** Result of applying one sequence to the authenticated receive high-water mark. */
enum class ReplayDecision : std::uint8_t {
    accepted,
    duplicate,
    tooOld,
};

/**
 * Strict authenticated receive high-water mark for one secure association.
 * This is a high-water mark, not a sliding window: only a sequence above the last accepted one is
 * admitted, so a reordered record is dropped rather than held.
 */
struct ReplayHighWater {
    /** Highest accepted expanded sequence. */
    std::uint32_t lastSequence{};
    /** False until the first sequence is accepted. */
    bool initialized{};
};

/**
 * Applies one expanded sequence to a strict authenticated high-water mark.
 * Gaps are accepted. Duplicates, older values, and numeric rollover are rejected.
 * @param state Receive history for one association.
 * @param sequence Expanded sequence to check.
 * @return The accept or rejection reason. Rejection does not change the state.
 */
[[nodiscard]] ReplayDecision update(ReplayHighWater& state, std::uint32_t sequence) noexcept;

} // namespace sunrise::middleware::gameplay::dtls
