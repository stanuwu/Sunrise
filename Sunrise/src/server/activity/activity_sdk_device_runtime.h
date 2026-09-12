#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../middleware/bap/activity_message/scriptable_auth_body.h"
#include "../../state/activity_sdk/runtime.h"
#include "../bap/runtime.h"

namespace sunrise::server::activity::host {
struct ScriptableOutputReservation;
}

namespace sunrise::server::activity::activity_sdk_devices {

/** Stable result of resolving or changing one exact generated type-23 device slot. */
enum class Status : std::uint8_t {
    ready,
    queued,
    invalidView,
    staleBinding,
    staleActivityClient,
    invalidSlot,
    wrongScenario,
    schemaJoinNotExact,
    wrongSdkBuild,
    invalidBody,
    invalidChannel,
    invalidValue,
    noActivityLink,
    targetUnavailable,
    ambiguousTarget,
    outputBusy,
    refused,
    /** The slot type faults the stock client from wire-legal values, so no body reaches it. */
    refusedSlotType,
};

/**
 * Maps the SDK binding validator to the stable refusal surface every SDK adapter shares.
 * @param view Pinned SDK view whose binding is revalidated.
 * @param link Cleared, then receives the exact ActivityClient generation and region.
 * @return ready only while one authenticated link owns the binding and reports its region.
 */
[[nodiscard]] Status live_binding_status(const state::activity_sdk::BoundView& view,
                                         server::bap::ActivityLinkView& link) noexcept;

/** Revalidates one value-owned generated Auth body without changing Host output state. */
[[nodiscard]] Status auth_availability(const state::activity_sdk::BoundView& view,
                                       std::uint32_t slotRow,
                                       std::uint32_t objectTag,
                                       std::uint32_t registryKey,
                                       std::uint32_t authSchema,
                                       std::uint16_t slotIndex,
                                       std::uint8_t slotType,
                                       std::span<const std::byte> body,
                                       std::uint16_t bitCount,
                                       std::span<const std::byte> sdkBuildSha256) noexcept;

/** Retains one type-2 actor channel for an operator action. */
[[nodiscard]] Status set_combatant_channel(const state::activity_sdk::BoundView& view,
                                           std::uint32_t slotRow,
                                           std::uint32_t channelHash,
                                           float value) noexcept;

/** Plays an extracted actor-owned sequence while preserving the combatant's Auth state. */
[[nodiscard]] Status play_combatant_sequence(const state::activity_sdk::BoundView& view,
                                             std::uint32_t slotRow,
                                             std::uint32_t sequenceRow) noexcept;

/** Queues an extracted sequence only on its captured owner and reserved Host revision. */
[[nodiscard]] Status
play_combatant_sequence_reserved(const state::activity_sdk::BoundView& view,
                                 const state::activity::mission::ActorSequenceOwner& expected,
                                 std::uint32_t sequenceRow,
                                 const host::ScriptableOutputReservation& reservation) noexcept;

/** Cancels the combatant's atom program without changing actor binding or control fields. */
[[nodiscard]] Status stop_combatant_sequence(const state::activity_sdk::BoundView& view,
                                             std::uint32_t slotRow) noexcept;

/** Arms one combatant for its scene's squad member spawn. */
[[nodiscard]] Status bind_combatant_to_squad(const state::activity_sdk::BoundView& view,
                                             std::uint32_t slotRow) noexcept;

/** Sends one compiled Auth body for an operator action, keyed only by the slot row. */
[[nodiscard]] Status apply_auth_slot(const state::activity_sdk::BoundView& view,
                                     std::uint32_t slotRow,
                                     std::span<const std::byte> body,
                                     std::uint16_t bitCount) noexcept;

/** Queues one exact compiled Auth body through an already owned durable Host revision. */
[[nodiscard]] Status
apply_auth_reserved(const state::activity_sdk::BoundView& view,
                    std::uint32_t slotRow,
                    std::uint32_t objectTag,
                    std::uint32_t registryKey,
                    std::uint32_t authSchema,
                    std::uint16_t slotIndex,
                    std::uint8_t slotType,
                    std::span<const std::byte> body,
                    std::uint16_t bitCount,
                    std::span<const std::byte> sdkBuildSha256,
                    const host::ScriptableOutputReservation& reservation) noexcept;

/**
 * Instantiates or removes package-authored type-4 entries, one push for the whole run.
 * A row is dropped, not refused, when the client's state does not hold it, or when it names a
 * second object. Only the first row's group reaches the roster, so a stranger discards the push.
 * @param extraRows Slot rows that share the reservation, so one push carries the whole run.
 * @return `queued` when at least one row resolved, else why the last one did not.
 */
[[nodiscard]] Status
set_objects_active_reserved(const state::activity_sdk::BoundView& view,
                            std::uint32_t slotRow,
                            std::span<const std::uint32_t> extraRows,
                            std::int32_t entryIndex,
                            bool active,
                            const host::ScriptableOutputReservation& reservation) noexcept;

/** Writes one named float channel into an authored actor when it attaches or restores. */
[[nodiscard]] Status
set_combatant_channel_reserved(const state::activity_sdk::BoundView& view,
                               std::uint32_t slotRow,
                               std::uint32_t channelHash,
                               float value,
                               const host::ScriptableOutputReservation& reservation) noexcept;

/** Arms one generated combatant for its scene's squad member spawn. */
[[nodiscard]] Status
bind_combatant_to_squad_reserved(const state::activity_sdk::BoundView& view,
                                 std::uint32_t slotRow,
                                 const host::ScriptableOutputReservation& reservation) noexcept;

/** Checks one exact SDK device request without changing Activity Host output state. */
[[nodiscard]] Status
availability(const state::activity_sdk::BoundView& view,
             std::uint32_t slotRow,
             middleware::bap::activity_message::scriptable_auth::Type23Channel channel,
             float value,
             bool snap) noexcept;

/** Queues one exact SDK device request through the typed activity message-5 route. */
[[nodiscard]] Status
set_channel(const state::activity_sdk::BoundView& view,
            std::uint32_t slotRow,
            middleware::bap::activity_message::scriptable_auth::Type23Channel channel,
            float value,
            bool snap) noexcept;

/** Queues only through the exact unarmed Host revision owned by Mission State. */
[[nodiscard]] Status
set_channel_reserved(const state::activity_sdk::BoundView& view,
                     std::uint32_t slotRow,
                     middleware::bap::activity_message::scriptable_auth::Type23Channel channel,
                     float value,
                     bool snap,
                     const host::ScriptableOutputReservation& reservation) noexcept;

/**
 * Checks one exact type-31 trigger request without changing Activity Host output state.
 * @param view Pinned SDK view whose binding is revalidated.
 * @param slotRow Catalog row of the type-31 slot the pulse targets.
 * @return ready only while the slot is an exact type-31 trigger on a live owned binding.
 */
[[nodiscard]] Status trigger_availability(const state::activity_sdk::BoundView& view,
                                          std::uint32_t slotRow) noexcept;

/** Fires one exact type-31 trigger through the typed activity message-5 route. */
[[nodiscard]] Status fire_trigger(const state::activity_sdk::BoundView& view,
                                  std::uint32_t slotRow) noexcept;

/**
 * Fires one type-31 trigger through an already owned durable Host revision.
 * The body carries no caller value. Its generation is minted by the Host from the per-target
 * guard, and the client fires the authored trigger/action graph from the auth body alone.
 * @param view Pinned SDK view whose binding is revalidated.
 * @param slotRow Catalog row of the type-31 slot the pulse targets.
 * @param reservation Host output revision this caller already owns.
 * @return queued once the pulse is staged, or the exact refusal.
 */
[[nodiscard]] Status
fire_trigger_reserved(const state::activity_sdk::BoundView& view,
                      std::uint32_t slotRow,
                      const host::ScriptableOutputReservation& reservation) noexcept;

/** @return Stable concise text for one device runtime result. */
[[nodiscard]] const char* status_name(Status status) noexcept;

} // namespace sunrise::server::activity::activity_sdk_devices
