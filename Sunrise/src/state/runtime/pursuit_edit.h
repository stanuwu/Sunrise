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
 * Replaces the saved row an objective's progress reads, provided it still holds the value the
 * editor read. An objective whose progress is not one saved flag or value row is refused, and so
 * is a bounty, because the edit does not read the deadline its held instance carries.
 * @param identity The held pursuit, on the selected character.
 * @param ordinal Objective position in the held pursuit's installed contract.
 * @param objectiveHash Definition hash the editor read at that position.
 * @param expected The row as the evaluator reads it: 0 or 1 for a flag.
 * @param value Replacement value, 0 or 1 for a flag.
 * @param change Receives the changed objects on success.
 * @param refusal Receives a static reason on failure.
 */
[[nodiscard]] bool set_pursuit_objective(const PursuitIdentity& identity,
                                         std::uint8_t ordinal,
                                         std::uint32_t objectiveHash,
                                         std::int32_t expected,
                                         std::int32_t value,
                                         AccountEditChange& change,
                                         const char** refusal = nullptr) noexcept;

/**
 * Writes the saved row of every required, incomplete objective to its target, in one savepoint.
 * The pursuit is never redeemed: its completion value and operations are left alone.
 * @param identity The held pursuit, on the selected character.
 * @param change Receives the changed objects on success.
 * @param refusal Receives a static reason on failure.
 */
[[nodiscard]] bool complete_pursuit_objectives(const PursuitIdentity& identity,
                                               AccountEditChange& change,
                                               const char** refusal = nullptr) noexcept;

} // namespace sunrise::state
