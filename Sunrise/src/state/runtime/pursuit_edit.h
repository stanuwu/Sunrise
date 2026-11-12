#pragma once

#include <cstdint>

#include "runtime.h"

namespace sunrise::state {

/**
 * The held pursuit an editor read. An edit is refused once the character, instance, hash or
 * serial no longer matches.
 */
struct PursuitIdentity {
    std::uint64_t characterSoid{};
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::int32_t mutationSerial{};
};

/**
 * Replaces an objective's saved progress, provided it still holds the value the editor read.
 * Progress is either one of the held copy's own values or the one saved flag or value row the
 * objective reads; any other objective is refused, and so is a bounty past its deadline.
 * @param identity The held pursuit, on the selected character.
 * @param ordinal Objective position in the held pursuit's installed contract.
 * @param objectiveHash Definition hash the editor read at that position.
 * @param expected The row as the evaluator reads it: 0 or 1 for a flag.
 * @param value Replacement value, 0 or 1 for a flag.
 * @param nowSeconds Server time in Unix seconds, compared with a bounty's deadline.
 * @param change Receives the changed objects on success.
 * @param refusal Receives a static reason on failure.
 */
[[nodiscard]] bool set_pursuit_objective(const PursuitIdentity& identity,
                                         std::uint8_t ordinal,
                                         std::uint32_t objectiveHash,
                                         std::int32_t expected,
                                         std::int32_t value,
                                         std::int64_t nowSeconds,
                                         AccountEditChange& change,
                                         const char** refusal = nullptr) noexcept;

/**
 * Writes the saved progress of every required, incomplete objective to its target, in one
 * savepoint. A bounty past its deadline is refused.
 * The pursuit is never redeemed: its completion value and operations are left alone.
 * @param identity The held pursuit, on the selected character.
 * @param nowSeconds Server time in Unix seconds, compared with a bounty's deadline.
 * @param change Receives the changed objects on success.
 * @param refusal Receives a static reason on failure.
 */
[[nodiscard]] bool complete_pursuit_objectives(const PursuitIdentity& identity,
                                               std::int64_t nowSeconds,
                                               AccountEditChange& change,
                                               const char** refusal = nullptr) noexcept;

} // namespace sunrise::state
