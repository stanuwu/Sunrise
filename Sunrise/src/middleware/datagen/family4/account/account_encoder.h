#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "../../../../state/account/account_state.h"
#include "../../../../state/build_data/constants/definition.h"
#include "../../../../state/unlocks/definition.h"

namespace sunrise::middleware::datagen::family4::account {

/** Account-object fields State holds outside the account copy and its unlock table. */
struct StateFields {
    /** Row of each character's value block that characterValues fills; unbound fills none. */
    std::uint16_t characterValueRow{state::build_data::constants::kUnboundValueRow};
    /** The row's value for each character, in roster order. */
    std::array<std::int32_t, state::kCharacterCapacity> characterValues{};
    /** Store wallet sync generation; zero while the account has never synced. */
    std::uint32_t storeSyncRevision{};
};

/**
 * Encodes a sentinel-correct account object from live State.
 * @param state Account identity, roster, preferences, and selected-character state.
 * @param fields Values State holds outside the account copy, from the same view as state.
 * @param output Exact State-mapped account-object storage.
 * @param publicOnly Publishes identity and roster while omitting private account banks.
 * @return True when State is valid and every required fixed region fits.
 */
[[nodiscard]] bool encode(const state::AccountState& state,
                          const StateFields& fields,
                          std::span<std::byte> output,
                          bool publicOnly = false) noexcept;

/**
 * Account-wide unlocks use the supplied snapshot; per-character flags still use saved state.
 * @param state Account identity, roster, preferences, and inventory to encode.
 * @param fields Values State holds outside the account copy, from the same view as state.
 * @param output Receives the account object; unchanged on failure.
 * @param unlocks Account unlocks from the same live or prepared view as state.
 * @return False when state, saved flags, mappings, or output bounds are invalid.
 */
[[nodiscard]] bool encode(const state::AccountState& state,
                          const StateFields& fields,
                          std::span<std::byte> output,
                          const state::unlocks::Table& unlocks) noexcept;

} // namespace sunrise::middleware::datagen::family4::account
