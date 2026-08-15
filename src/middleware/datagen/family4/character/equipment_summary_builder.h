#pragma once

#include "../../../../state/equipment/light/definition.h"
#include "layout.h"

namespace sunrise::middleware::datagen::family4::character {

/**
 * Maps a complete semantic light evaluation into the wire ABI.
 * @param evaluation Independently calculated profile, character, and aggregate values.
 * @param output Receives a complete 340-byte equipment summary only on success.
 * @return True when every semantic entry and aggregate field is internally consistent.
 */
[[nodiscard]] bool build_equipment_summary(const state::equipment::light::Evaluation& evaluation,
                                           layout::EquipmentSummary& output) noexcept;

} // namespace sunrise::middleware::datagen::family4::character
