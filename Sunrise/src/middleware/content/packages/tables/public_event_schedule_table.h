#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../../state/build_data/public_event_schedule/definition.h"

namespace sunrise::middleware::content::packages::tables::public_event_schedule {
/** investment_globals names the Director's public-event schedule table as this child. */
inline constexpr std::size_t kGlobalsChild = 2;

/**
 * Decodes the schedule table's payload: rows of period, jitter span, offset and window in authored
 * minutes with their weighted entries, and the destination groups the map draws.
 * @param payload Exactly the tag's bytes.
 * @param tag The tag the payload came from, recorded for provenance.
 * @param table Receives the decoded table; left unspecified when the payload is refused.
 * @return False when any count or offset leaves the payload or exceeds the store's capacity.
 */
[[nodiscard]] bool decode(std::span<const std::byte> payload,
                          std::uint32_t tag,
                          state::build_data::public_event_schedule::Table& table) noexcept;
} // namespace sunrise::middleware::content::packages::tables::public_event_schedule
