#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "../../middleware/bap/activity_message/squad_auth_body.h"
#include "../../state/activity/mission/definition.h"
#include "../../state/activity_sdk/runtime.h"
#include "../../state/gameplay/external/squad_entity_retirement.h"

namespace sunrise::server::activity::host {
struct ScriptableOutputReservation;
struct ScriptableTarget;
} // namespace sunrise::server::activity::host

namespace sunrise::server::activity::activity_sdk_squads {

/** Stable result of resolving or placing one generated SDK squad row. */
enum class Status : std::uint8_t {
    ready,
    queued,
    invalidView,
    staleBinding,
    staleActivityClient,
    invalidSquad,
    wrongScenario,
    notRunnable,
    invalidMode,
    memberCountMismatch,
    memberCountOutOfRange,
    noActivityLink,
    targetUnavailable,
    ambiguousTarget,
    outputBusy,
    refused,
};

/**
 * Checks one exact SDK squad request without mutating Activity Host output state.
 * @param destinationSquadRow Catalog row of a type-1 squad in the same object that member
 * actor-spawn actions fill, or none.
 * @param spawnRuleSlotRow Catalog slot row of a type-66 rule in the same object that replaces
 * the package rule, or none.
 */
[[nodiscard]] Status
availability(const state::activity_sdk::BoundView& view,
             std::uint32_t squadRow,
             std::span<const std::int32_t> requestedCounts,
             middleware::bap::activity_message::squad_auth::Mode mode,
             std::optional<std::uint32_t> nameHash = std::nullopt,
             bool retireOnReturn = false,
             std::optional<std::uint32_t> destinationSquadRow = std::nullopt,
             std::optional<std::uint32_t> spawnRuleSlotRow = std::nullopt) noexcept;

/** Queues one exact generated SDK squad request through the existing activity msg-5 route. */
[[nodiscard]] Status place(const state::activity_sdk::BoundView& view,
                           std::uint32_t squadRow,
                           std::span<const std::int32_t> requestedCounts,
                           middleware::bap::activity_message::squad_auth::Mode mode,
                           std::optional<std::uint32_t> nameHash = std::nullopt,
                           bool retireOnReturn = false,
                           std::optional<std::uint32_t> destinationSquadRow = std::nullopt,
                           std::optional<std::uint32_t> spawnRuleSlotRow = std::nullopt) noexcept;

/**
 * Queues only through the exact unarmed Host revision owned by Mission State.
 * @param temperaments The squad's temperament list; absent keeps the one it last received.
 */
[[nodiscard]] Status place_reserved(
    const state::activity_sdk::BoundView& view,
    std::uint32_t squadRow,
    std::span<const std::int32_t> requestedCounts,
    middleware::bap::activity_message::squad_auth::Mode mode,
    const host::ScriptableOutputReservation& reservation,
    std::optional<std::uint32_t> nameHash = std::nullopt,
    bool retireOnReturn = false,
    std::optional<std::uint32_t> destinationSquadRow = std::nullopt,
    std::optional<std::uint32_t> spawnRuleSlotRow = std::nullopt,
    std::optional<middleware::bap::activity_message::squad_auth::Temperaments> temperaments =
        std::nullopt) noexcept;

/**
 * Reads the bubble a squad's authored occurrence belongs to, which names the client that can
 * instantiate its members. It reads the package alone, so it holds on every link.
 * @param bubble Receives the authored bubble ordinal; zero on refusal.
 * @return ready; invalidView without a catalog; wrongScenario for another scenario's squad;
 * invalidSquad for a row, occurrence or bubble outside the catalog.
 */
[[nodiscard]] Status authored_bubble(const state::activity_sdk::BoundView& view,
                                     std::uint32_t squadRow,
                                     std::uint8_t& bubble) noexcept;

/** @return Stable concise text for one squad runtime result. */
[[nodiscard]] const char* status_name(Status status) noexcept;

/**
 * The host retires the actors a client gives up on a bubble leave when their delivery opts in.
 * @param source The squad, or the combatant its member binds to.
 * @param counts One requested count per member slot of the squad at `squadRow`.
 * @return Eligibility naming `source`, enabled only for one exact actor class.
 */
[[nodiscard]] state::gameplay::squad_entity_retirement::Eligibility
retirement_eligibility(const state::activity_sdk::BoundView& view,
                       std::uint32_t squadRow,
                       std::span<const std::int32_t> counts,
                       const host::ScriptableTarget& source,
                       bool enabled) noexcept;

/** @return The temperament list a script gave a squad, or none to keep the squad's last one. */
[[nodiscard]] inline std::optional<middleware::bap::activity_message::squad_auth::Temperaments>
intent_temperaments(const state::activity::mission::TypedIntent& intent) noexcept {
    namespace squad_auth = middleware::bap::activity_message::squad_auth;
    static_assert(state::activity::mission::kSquadTemperamentCapacity
                  == squad_auth::kTemperamentCapacity);
    if (!intent.squadTemperamentsSet) {
        return std::nullopt;
    }
    squad_auth::Temperaments temperaments{};
    temperaments.count = intent.squadTemperamentCount;
    for (std::size_t index = 0; index < intent.squadTemperamentCount; ++index) {
        temperaments.ids[index].value = intent.squadTemperaments[index];
    }
    return temperaments;
}

} // namespace sunrise::server::activity::activity_sdk_squads
