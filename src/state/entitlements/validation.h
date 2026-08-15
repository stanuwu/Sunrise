#pragma once

#include <cstdint>

#include "definition.h"

namespace sunrise::state::entitlements {

/**
 * Checks one authored entitlement definition.
 * @param candidate Candidate definition.
 * @return True when the name is bounded printable ASCII and the ownership form fits it.
 */
[[nodiscard]] bool valid(const Entitlement& candidate) noexcept;

/**
 * Checks one complete authored ownership table.
 * @param candidate Candidate table.
 * @return True when the table is bounded and every definition is valid and uniquely named.
 */
[[nodiscard]] bool valid(const Table& candidate) noexcept;

/**
 * Finds the id one owned definition is declared by.
 * @param table Already-checked authored policy.
 * @param index Zero-based table index.
 * @param identifier Receives the manifest handle or the numeric application id.
 * @return False when the definition is not owned.
 */
[[nodiscard]] bool
owned_identifier(const Table& table, std::size_t index, std::uint32_t& identifier) noexcept;

/** @return The bundled ownership policy used when settings supply none. */
[[nodiscard]] Table authored() noexcept;

} // namespace sunrise::state::entitlements
