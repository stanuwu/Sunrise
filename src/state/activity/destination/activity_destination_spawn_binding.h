#pragma once

#include <cstdint>

#include "definition.h"

namespace sunrise::state::activity::destination {

/**
 * Picks the spawn-set hash to send, dropping one the destination does not load.
 * A set declared only by a package the destination never loads cannot attach, and the player
 * arrives with no spawn point. The absent hash goes out instead, so the Client picks its own.
 * @param selection Committed destination.
 * @param fallback Authored fallback hash.
 * @return The resolved hash, or the absent hash when the set cannot attach.
 */
[[nodiscard]] std::uint32_t attachable_spawn_set_hash(const DestinationSelection& selection,
                                                      std::uint32_t fallback) noexcept;

} // namespace sunrise::state::activity::destination
