#pragma once

#include <span>

#include "../definition.h"

namespace sunrise::state::equipment::light::calculation {

/**
 * Computes raw summary arrays and the selected character's merged weighted light values.
 * @param selectedCharacter Raw scores for the selected character.
 * @param profileSlotMaxima Raw profile-owned maximum scores.
 * @param otherCharacterScores Raw score arrays for every other character.
 * @param output Destination replaced only after a complete valid calculation.
 * @return True when the signed total fits the summary's 32-bit fields.
 */
[[nodiscard]] bool evaluate(const SlotScores& selectedCharacter,
                            const SlotScores& profileSlotMaxima,
                            std::span<const SlotScores> otherCharacterScores,
                            Evaluation& output) noexcept;

} // namespace sunrise::state::equipment::light::calculation
