#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "../../middleware/bap/activity_message/squad_auth_body.h"
#include "../../state/activity_sdk/runtime.h"

namespace sunrise::server::activity::host {
struct ScriptableOutputReservation;
}

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

/** Queues only through the exact unarmed Host revision owned by Mission State. */
[[nodiscard]] Status
place_reserved(const state::activity_sdk::BoundView& view,
               std::uint32_t squadRow,
               std::span<const std::int32_t> requestedCounts,
               middleware::bap::activity_message::squad_auth::Mode mode,
               const host::ScriptableOutputReservation& reservation,
               std::optional<std::uint32_t> nameHash = std::nullopt,
               bool retireOnReturn = false,
               std::optional<std::uint32_t> destinationSquadRow = std::nullopt,
               std::optional<std::uint32_t> spawnRuleSlotRow = std::nullopt) noexcept;

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

} // namespace sunrise::server::activity::activity_sdk_squads
