#pragma once

#include <span>

#include "../../../../state/account/account_state.h"
#include "../../../../state/unlocks/definition.h"

namespace sunrise::middleware::datagen::family4::account {

/**
 * Encodes a sentinel-correct account object from live State.
 * @param state Account identity, roster, preferences, and selected-character state.
 * @param output Exact State-mapped account-object storage.
 * @return True when State is valid and every required fixed region fits.
 */
[[nodiscard]] bool encode(const state::AccountState& state, std::span<std::byte> output) noexcept;

/**
 * Account-wide unlocks use the supplied snapshot; per-character flags still use saved state.
 * @param state Account identity, roster, preferences, and inventory to encode.
 * @param output Receives the account object; unchanged on failure.
 * @param unlocks Account unlocks from the same live or prepared view as state.
 * @return False when state, saved flags, mappings, or output bounds are invalid.
 */
[[nodiscard]] bool encode(const state::AccountState& state,
                          std::span<std::byte> output,
                          const state::unlocks::Table& unlocks) noexcept;

} // namespace sunrise::middleware::datagen::family4::account
