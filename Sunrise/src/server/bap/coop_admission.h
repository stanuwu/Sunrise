#pragma once

#include <cstdint>

#include "../../state/runtime/state.h"

namespace sunrise::server::bap::coop_admission {

// A guest is admitted when its key report arrives over its encrypted Sunrise link, which proves
// the join code. Its native links then come only from the same address, and the host wraps their
// session keys with the reported keys. Callers hold the BAP lock.

/** Records one guest's sign-on keys and the address it reported them from. */
[[nodiscard]] bool
admit(std::uint64_t primarySoid, std::uint32_t address, const state::SignOnState& keys) noexcept;

/**
 * Copies the sign-on keys of a guest admitted from `address`, host byte order.
 * @return False when the guest was never admitted, or from another address.
 */
[[nodiscard]] bool
keys(std::uint64_t primarySoid, std::uint32_t address, state::SignOnState& output) noexcept;

} // namespace sunrise::server::bap::coop_admission