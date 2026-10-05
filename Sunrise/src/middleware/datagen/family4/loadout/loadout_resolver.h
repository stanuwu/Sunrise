#pragma once

#include <cstddef>

#include "../../../../state/account/account_state.h"
#include "definition.h"

namespace sunrise::middleware::datagen::family4::loadout {

/**
 * Resolves one selected character's authored equipment through installed build data.
 * @param account Validated lock-consistent account State.
 * @param selectedCharacterIndex Selected row within the occupied character array.
 * @param output Receives a complete row-sorted loadout only on success.
 * @return True when every authored item, bucket, socket, and semantic slot resolves.
 */
[[nodiscard]] bool resolve(const state::AccountState& account,
                           std::size_t selectedCharacterIndex,
                           ResolvedLoadout& output) noexcept;

/** Resolves a public inspection row without changing the account's selected character. */
[[nodiscard]] bool resolve_character(const state::AccountState& account,
                                     std::size_t characterIndex,
                                     ResolvedLoadout& output) noexcept;

/**
 * Resolves the item instances one character owns, selected or not.
 * The equip-summary reader looks up an instance without a null check, so every character the
 * roster names needs its records even when its loadout is never encoded.
 * @param account Validated lock-consistent account State.
 * @param characterIndex Row within the occupied character array.
 * @param output Receives every resolved instance only on success.
 * @return True when every authored item on that character resolves.
 */
[[nodiscard]] bool resolve_instances(const state::AccountState& account,
                                     std::size_t characterIndex,
                                     ResolvedInstances& output) noexcept;

/**
 * Resolves every equipped and unequipped item instance one character owns.
 * Family four must
 * publish an instance object for every SOID referenced by the character inventory.
 */
[[nodiscard]] bool resolve_owned_instances(const state::AccountState& account,
                                           std::size_t characterIndex,
                                           ResolvedInstances& output) noexcept;

} // namespace sunrise::middleware::datagen::family4::loadout
