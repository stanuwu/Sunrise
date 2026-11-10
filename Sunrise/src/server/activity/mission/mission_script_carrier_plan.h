#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "../../../middleware/bap/activity_message/combatant_auth.h"
#include "../../../state/activity/mission/definition.h"

namespace sunrise::server::activity::mission {

/** How a carrier reaches the point it unloads at. */
enum class CarrierEntryMode : std::uint8_t {
    /** Along the entry path from its start. */
    path,
    /** By the entry ability, from the entry path's last point. */
    ability,
    /** By the entry ability from the entry path's start, then along the path. */
    abilityThenPath,
    /** Along the entry path, then by the entry ability. */
    pathThenAbility,
};

/** One authored type-58 path by its ClientRef and the number of points it holds. */
struct CarrierPath final {
    middleware::bap::activity_message::scriptable_auth::Type2LaneClientRef reference{};
    std::uint8_t points{};
};

/** One squad a carrier reserves and unloads, with the counts its reservation holds. */
struct CarrierCargo final {
    std::array<std::int32_t, state::activity::mission::kSquadMemberCapacity> counts{};
    std::uint32_t squadRow{};
    middleware::bap::activity_message::combatant_auth::SquadReference reference{};
    std::uint8_t memberCount{};
};

/** One validated start_carrier call. */
struct CarrierDefinition final {
    /** The actor's identity, SDK build and authored source, which every actor step carries. */
    state::activity::mission::TypedIntent actor{};
    CarrierPath entry{};
    CarrierPath exit{};
    middleware::bap::activity_message::scriptable_auth::Type2LaneTripleRef entryAbility{};
    middleware::bap::activity_message::scriptable_auth::Type2LaneTripleRef exitAbility{};
    std::array<CarrierCargo,
               middleware::bap::activity_message::combatant_auth::kMaximumManifestSquads>
        cargo{};
    /** Temperaments the actor holds from its creation to its retirement. */
    middleware::bap::activity_message::scriptable_auth::Type2ChannelState controls{};
    std::uint32_t doorChannel{};
    /** The one exit point the actor leaves for, or -1 for every exit point in order. */
    std::int16_t exitMarker{-1};
    std::uint8_t cargoCount{};
    CarrierEntryMode entryMode{};
    bool relocateStart{};
    bool serial{};
};

/**
 * Composes a flight from native steps: creation and entry, open doors, reserve and unload the
 * cargo, close doors, depart, exit ability, retirement. Every counter is a template the Host
 * replaces when the step is delivered.
 * @return The plan, or null when a step does not fit its native body. Throws std::bad_alloc.
 */
[[nodiscard]] std::shared_ptr<const state::activity::mission::CarrierPlan>
compile_carrier_plan(const CarrierDefinition& definition);

} // namespace sunrise::server::activity::mission
