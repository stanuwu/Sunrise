#pragma once

#include <cstdint>

#include "../../account/account_state.h"

namespace sunrise::state::runtime::equipment {

/**
 * Builds a nonsecret cache identity from ordered authored equipment.
 * @param accountState Checked account with canonical character and semantic-slot order.
 * @return Stable presence, base-hash, and authored-level fingerprint.
 */
[[nodiscard]] std::uint64_t configured_hash(const AccountState& accountState) noexcept;

} // namespace sunrise::state::runtime::equipment
