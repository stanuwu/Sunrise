#pragma once

#include <cstdint>

namespace sunrise::server::bap::coop_admission {

// A guest's own Sunrise link passes the compatibility check; its native links then come only from
// the same address. Callers hold the BAP lock.

/** Records that `primarySoid` passed the check from `address`. @return False when full. */
[[nodiscard]] bool admit(std::uint64_t primarySoid, std::uint32_t address) noexcept;

/** @return True when `primarySoid` passed the check from `address`, host byte order. */
[[nodiscard]] bool admitted(std::uint64_t primarySoid, std::uint32_t address) noexcept;

/** Forgets every admission. */
void reset() noexcept;

} // namespace sunrise::server::bap::coop_admission