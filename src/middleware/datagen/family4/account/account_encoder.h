#pragma once

#include <span>

#include "../../../../state/account/account_state.h"

namespace sunrise::middleware::datagen::family4::account {

/**
 * Encodes a sentinel-correct account object from authored State.
 * @param state Account identity, roster, preferences, and selected-character state.
 * @param output Exact State-mapped account-object storage.
 * @return True when State is valid and every required fixed region fits.
 */
[[nodiscard]] bool encode(const state::AccountState& state, std::span<std::byte> output) noexcept;

} // namespace sunrise::middleware::datagen::family4::account
