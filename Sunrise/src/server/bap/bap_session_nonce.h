#pragma once
#include <array>
#include <cstdint>

#include "../../state/runtime/state.h"

namespace sunrise::server::bap {
/** Borrows the live nonce while the caller holds session_lock() exclusively. */
[[nodiscard]] std::array<std::byte, state::kBapNonceSize>*
downstream_send_nonce(std::uint32_t connectionId) noexcept;
} // namespace sunrise::server::bap
