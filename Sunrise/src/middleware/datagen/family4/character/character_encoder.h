#pragma once

#include <span>

#include "../../../../state/account/account_state.h"
#include "../../../../state/equipment/light/definition.h"
#include "../../../../state/unlocks/definition.h"
#include "../loadout/definition.h"

namespace sunrise::middleware::datagen::family4::character {

/**
 * Encodes one selected-character object from live State and resolved installed mappings.
 * @param state Validated authored character identity and policy state.
 * @param resolvedLoadout Row-sorted inventory and equipment mappings for this character.
 * @param lightEvaluation Complete raw and aggregate equipment-light values.
 * @param output Exact runtime-mapped character-object storage.
 * @return True when State, mappings, and the mapped object span fit the native layout.
 */
[[nodiscard]] bool encode(const state::CharacterState& state,
                          const loadout::ResolvedLoadout& resolvedLoadout,
                          const state::equipment::light::Evaluation& lightEvaluation,
                          std::span<std::byte> output) noexcept;

/**
 * Character unlocks must match the live or prepared inventory view being encoded.
 * @param state Character identity and inventory to encode.
 * @param resolvedLoadout Item mappings for this character's inventory.
 * @param lightEvaluation Equipment light values for the same loadout.
 * @param output Receives the character object; unchanged on failure.
 * @param unlocks Unlock snapshot for this character, including any prepared quest value.
 * @return False when state, mappings, light values, or output bounds are invalid.
 */
[[nodiscard]] bool encode(const state::CharacterState& state,
                          const loadout::ResolvedLoadout& resolvedLoadout,
                          const state::equipment::light::Evaluation& lightEvaluation,
                          std::span<std::byte> output,
                          const state::unlocks::Table& unlocks) noexcept;

} // namespace sunrise::middleware::datagen::family4::character
