#pragma once

#include "../destination/definition.h"
#include "definition.h"

namespace sunrise::state::activity::forced {

/**
 * Replaces the forced destination.
 * @param value Candidate selection, complete or partial.
 * @return True when every named field is inside its wire range and the value was stored.
 */
[[nodiscard]] bool publish(const ForcedDestination& value) noexcept;

/**
 * Copies the forced destination.
 * @param value Receives the stored selection.
 */
void snapshot(ForcedDestination& value) noexcept;

/** Drops the selection and the switch, the same as the interface's clear action. */
void clear() noexcept;

/**
 * Overwrites one committed destination with the forced one. The descriptor bits are dropped:
 * they carry the client's chosen name, and one outbound message replays them as-is. The
 * activity index goes too, because many package names map to several definitions.
 * @param selection Destination built from the client's request, replaced in place.
 * @return True when a complete forced destination was applied.
 */
[[nodiscard]] bool apply(destination::DestinationSelection& selection) noexcept;

} // namespace sunrise::state::activity::forced
