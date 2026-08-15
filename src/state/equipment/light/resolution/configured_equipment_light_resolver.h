#pragma once

#include <cstddef>

#include "../../../account/account_state.h"
#include "../definition.h"

namespace sunrise::state::equipment::light::resolution {

/**
 * Finds authored equipment in the installed item and detail maps, then computes light from the
 * authored item levels.
 * @param account Already-checked account configuration, read under the lock.
 * @param selectedCharacterIndex Selected used character row.
 * @param output Receives a complete selected-character evaluation only on success.
 * @return True when every configured item has one consistent native slot and exact score.
 */
[[nodiscard]] bool resolve(const AccountState& account,
                           std::size_t selectedCharacterIndex,
                           Evaluation& output) noexcept;

/**
 * Computes the equipment light one character displays, selected or not.
 * @param account Already-checked account configuration, read under the lock.
 * @param characterIndex Used character row.
 * @param light Receives the weighted integer average.
 * @return True when every item on that character is found.
 */
[[nodiscard]] bool character_light(const AccountState& account,
                                   std::size_t characterIndex,
                                   std::int32_t& light) noexcept;

} // namespace sunrise::state::equipment::light::resolution
