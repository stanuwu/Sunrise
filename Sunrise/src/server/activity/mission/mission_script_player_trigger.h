#pragma once

#include <cstdint>

#include "../../../middleware/bap/activity_message/player_trigger_incident.h"
#include "../../../state/build_data/scriptables/definition.h"

namespace sunrise::server::activity::mission::player_trigger {

enum class ResolveStatus : std::uint8_t {
    ready,
    absent,
    ambiguous,
    invalidCatalog,
};

/** Exact authored type-31 source and its active type-60 target. */
struct Source final {
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint32_t volumeRegistryKey{};
    std::uint32_t sourceObjectRow{};
    std::uint32_t sourceSlotRow{};
    std::uint16_t volumeSlotIndex{};
    std::uint16_t slotIndex{};
    std::uint8_t volumeSlotType{};
    std::uint8_t slotType{};
};

/**
 * Resolves one type-31 incident and its generated type-60 target.
 * @param region Region the volume's occurrence must lie in, or kAnyRegion.
 */
[[nodiscard]] ResolveStatus
resolve(const state::build_data::scriptables::Snapshot& snapshot,
        const middleware::bap::activity_message::player_trigger_incident::Payload& payload,
        Source& output,
        std::int32_t region) noexcept;

} // namespace sunrise::server::activity::mission::player_trigger
