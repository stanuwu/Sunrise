#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <string_view>

#include "../../middleware/bap/activity_message/damage_monitor_auth.h"
#include "../../middleware/bap/activity_message/darkness_zone_auth.h"
#include "../../middleware/bap/activity_message/ghost_link_auth.h"
#include "../../middleware/bap/activity_message/interactable_object_auth.h"
#include "../../middleware/bap/activity_message/mission_effect_auth.h"
#include "../../middleware/bap/activity_message/music_section_auth.h"
#include "../../middleware/bap/activity_message/scene_events_auth.h"
#include "../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../middleware/bap/activity_message/squad_objective_auth.h"
#include "../../middleware/content/packages/tables/region_reader.h"
#include "../../state/activity/runtime.h"
#include "../../state/activity_sdk/actor_sequences.h"
#include "../../state/build_data/runtime.h"
#include "../bap/runtime.h"
#include "activity_sdk_actor_sequences.h"
#include "activity_sdk_device_internal.h"
#include "host_runtime.h"

namespace sunrise::server::activity::activity_sdk_devices {
namespace {

namespace auth = middleware::bap::activity_message::scriptable_auth;
namespace format = state::activity_sdk::format;
namespace layouts = state::build_data::scenarios;
namespace sdk = state::activity_sdk;
namespace tables = middleware::content::packages::tables;

static_assert(format::kDeviceSlotType == auth::kType23SlotType);
static_assert(format::kDeviceAuthSchema == auth::kType23Schema);
static_assert(format::kObjectSlotType == auth::kType4SlotType);
static_assert(format::kObjectAuthSchema == auth::kType4Schema);

using detail::binding_status;
using detail::prepare;
using detail::prepare_combatant;
using detail::prepare_object;
using detail::prepare_slot;
using detail::prepare_trigger;
using detail::PreparedDevice;
using detail::validate_auth;

} // namespace

Status live_binding_status(const sdk::BoundView& view,
                           server::bap::ActivityLinkView& link) noexcept {
    return binding_status(view, link);
}

/** @return Whether one device Auth slot can take a body right now. */
Status auth_availability(const sdk::BoundView& view,
                         std::uint32_t slotRow,
                         std::uint32_t objectTag,
                         std::uint32_t registryKey,
                         std::uint32_t authSchema,
                         std::uint16_t slotIndex,
                         std::uint8_t slotType,
                         std::span<const std::byte> body,
                         std::uint16_t bitCount,
                         std::span<const std::byte> sdkBuildSha256) noexcept {
    const Status validated = validate_auth(view,
                                           slotRow,
                                           objectTag,
                                           registryKey,
                                           authSchema,
                                           slotIndex,
                                           slotType,
                                           body,
                                           bitCount,
                                           sdkBuildSha256);
    if (validated != Status::ready) {
        return validated;
    }
    PreparedDevice prepared{};
    return prepare_slot(view, slotRow, prepared);
}

/**
 * Sends one compiled Auth body for an operator action, taking every identity from the slot row.
 * The reserved form is for Mission, which owns a revision; an operator owns none.
 */
Status apply_auth_slot(const sdk::BoundView& view,
                       std::uint32_t slotRow,
                       std::span<const std::byte> body,
                       std::uint16_t bitCount) noexcept {
    if (view.catalog == nullptr) {
        return Status::invalidView;
    }
    const sdk::Catalog& catalog = *view.catalog;
    const auto slots = catalog.slots();
    const auto objects = catalog.objects();
    if (slotRow >= slots.size()) {
        return Status::invalidSlot;
    }
    const sdk::format::Slot& slot = slots[slotRow];
    if (slot.objectIndex >= objects.size()
        || slot.slotIndex > (std::numeric_limits<std::uint16_t>::max)()
        || slot.slotType > (std::numeric_limits<std::uint8_t>::max)()) {
        return Status::invalidSlot;
    }
    const sdk::format::Object& object = objects[slot.objectIndex];
    const Status validated = validate_auth(view,
                                           slotRow,
                                           object.objectTag,
                                           object.objectKey,
                                           slot.authSchema,
                                           static_cast<std::uint16_t>(slot.slotIndex),
                                           static_cast<std::uint8_t>(slot.slotType),
                                           body,
                                           bitCount,
                                           catalog.sdk_build_sha256());
    if (validated != Status::ready) {
        return validated;
    }
    PreparedDevice prepared{};
    const Status status = prepare_slot(view, slotRow, prepared);
    if (status != Status::ready) {
        return status;
    }
    return server::bap::request_activity_sdk_auth_override(
               view.binding,
               prepared.target,
               prepared.target.stateLocalRoster ? &prepared.generatedRosterGroup : nullptr,
               body,
               bitCount,
               prepared.effectiveRegion,
               prepared.activityClientGeneration)
               ? Status::queued
               : Status::refused;
}

/** Applies one device Auth body against an already-reserved Host output revision. */
Status apply_auth_reserved(const sdk::BoundView& view,
                           std::uint32_t slotRow,
                           std::uint32_t objectTag,
                           std::uint32_t registryKey,
                           std::uint32_t authSchema,
                           std::uint16_t slotIndex,
                           std::uint8_t slotType,
                           std::span<const std::byte> body,
                           std::uint16_t bitCount,
                           std::span<const std::byte> sdkBuildSha256,
                           const host::ScriptableOutputReservation& reservation) noexcept {
    const Status validated = validate_auth(view,
                                           slotRow,
                                           objectTag,
                                           registryKey,
                                           authSchema,
                                           slotIndex,
                                           slotType,
                                           body,
                                           bitCount,
                                           sdkBuildSha256);
    if (validated != Status::ready) {
        return validated;
    }
    PreparedDevice prepared{};
    const Status status = prepare_slot(view, slotRow, prepared);
    if (status != Status::ready) {
        return status;
    }
    const bool queued = server::bap::request_activity_sdk_auth_override(
        view.binding,
        prepared.target,
        prepared.target.stateLocalRoster ? &prepared.generatedRosterGroup : nullptr,
        body,
        bitCount,
        prepared.effectiveRegion,
        prepared.activityClientGeneration,
        &reservation);
    if (queued) {
        return Status::queued;
    }
    return Status::refused;
}

/** Sets the active set of one device's objects against a reserved Host output revision. */
Status set_objects_active_reserved(const sdk::BoundView& view,
                                   std::uint32_t slotRow,
                                   std::span<const std::uint32_t> extraRows,
                                   std::int32_t entryIndex,
                                   bool active,
                                   const host::ScriptableOutputReservation& reservation) noexcept {
    if (extraRows.size() > state::activity::mission::kIntentBurstCapacity) {
        return Status::refused;
    }
    // A run names one bubble's slots, and a bubble holds slots the client's state does not.
    // Refusing the whole run for one absent slot left the bubble empty, and one slot per push
    // made the 73 bazaar objects appear over four seconds.

    // The first row that resolves owns the reserved revision and every later body answers under
    // it, so one push carries the run. Only that row's group is installed into the roster the
    // push builds, so a row from a second object is dropped too: it would discard the push.
    Status skipped = Status::ready;
    bool head = false;
    host::ScriptableTarget group{};
    const auto queue = [&](std::uint32_t row) noexcept {
        PreparedDevice prepared{};
        const Status status = prepare_object(view, row, entryIndex, prepared);
        const bool sameGroup = !head
                               || (prepared.target.objectTag == group.objectTag
                                   && prepared.target.registryKey == group.registryKey);
        if (status != Status::ready || !prepared.target.stateLocalRoster || !sameGroup) {
            if (skipped == Status::ready) {
                skipped = status == Status::ready ? Status::targetUnavailable : status;
            }
            return true;
        }
        const bool queued =
            host::request_state_local_type4_override(view.binding,
                                                     prepared.target,
                                                     prepared.generatedRosterGroup,
                                                     entryIndex,
                                                     active,
                                                     prepared.activityClientGeneration,
                                                     head ? nullptr : &reservation,
                                                     head ? &reservation : nullptr);
        if (queued && !head) {
            group = prepared.target;
            head = true;
        }
        return queued;
    };
    if (!queue(slotRow)) {
        return Status::refused;
    }
    for (const std::uint32_t row : extraRows) {
        if (!queue(row)) {
            return Status::refused;
        }
    }
    return head ? Status::queued : skipped;
}

/**
 * Retains one type-2 actor channel against a reservation a script already holds.
 * @param reservation Output slot the caller reserved for this revision.
 * @return `queued` once the override is staged, or the refusal that stopped it.
 */
Status
set_combatant_channel_reserved(const sdk::BoundView& view,
                               std::uint32_t slotRow,
                               std::uint32_t channelHash,
                               float value,
                               const host::ScriptableOutputReservation& reservation) noexcept {
    PreparedDevice prepared{};
    const Status status = prepare_combatant(view, slotRow, prepared);
    if (status != Status::ready) {
        return status;
    }
    if (!prepared.target.stateLocalRoster) {
        return Status::targetUnavailable;
    }
    return host::request_state_local_type2_channel_override(view.binding,
                                                            prepared.target,
                                                            prepared.generatedRosterGroup,
                                                            channelHash,
                                                            value,
                                                            prepared.activityClientGeneration,
                                                            &reservation)
               ? Status::queued
               : Status::refused;
}

/** Retains one type-2 actor channel for an operator action, which owns no revision. */
Status set_combatant_channel(const sdk::BoundView& view,
                             std::uint32_t slotRow,
                             std::uint32_t channelHash,
                             float value) noexcept {
    PreparedDevice prepared{};
    const Status status = prepare_combatant(view, slotRow, prepared);
    if (status != Status::ready) {
        return status;
    }
    if (!prepared.target.stateLocalRoster) {
        return Status::targetUnavailable;
    }
    return host::request_state_local_type2_channel_override(view.binding,
                                                            prepared.target,
                                                            prepared.generatedRosterGroup,
                                                            channelHash,
                                                            value,
                                                            prepared.activityClientGeneration)
               ? Status::queued
               : Status::refused;
}

/** Plays a sequence only through the current client's exact generated combatant route. */
Status play_combatant_sequence(const sdk::BoundView& view,
                               std::uint32_t slotRow,
                               std::uint32_t sequenceRow) noexcept {
    actor_sequences::Owner owner{};
    if (!actor_sequences::owner(view, slotRow, owner)) {
        return Status::invalidValue;
    }
    const auto* const sequence =
        sdk::resolve_actor_sequence(*view.catalog, owner.actorClassRow, sequenceRow);
    if (sequence == nullptr || !sdk::actor_sequence_playable(*sequence)) {
        return Status::invalidValue;
    }
    PreparedDevice prepared{};
    const Status status = prepare_combatant(view, slotRow, prepared);
    if (status != Status::ready) {
        return status;
    }
    if (!prepared.target.stateLocalRoster) {
        return Status::targetUnavailable;
    }
    return host::request_state_local_type2_sequence(view.binding,
                                                    prepared.target,
                                                    prepared.generatedRosterGroup,
                                                    sequence->keyHash,
                                                    prepared.activityClientGeneration)
               ? Status::queued
               : Status::refused;
}

/** Resolves the captured owner again before reserving a native atom program. */
Status
play_combatant_sequence_reserved(const sdk::BoundView& view,
                                 const state::activity::mission::ActorSequenceOwner& expected,
                                 std::uint32_t sequenceRow,
                                 const host::ScriptableOutputReservation& reservation) noexcept {
    actor_sequences::Owner owner{};
    if (!actor_sequences::owner(view, expected.slotRow, owner) || owner != expected) {
        return Status::staleActivityClient;
    }
    const auto* const sequence =
        sdk::resolve_actor_sequence(*view.catalog, owner.actorClassRow, sequenceRow);
    if (sequence == nullptr || !sdk::actor_sequence_playable(*sequence)) {
        return Status::invalidValue;
    }
    PreparedDevice prepared{};
    const Status status = prepare_combatant(view, expected.slotRow, prepared);
    if (status != Status::ready) {
        return status;
    }
    if (!prepared.target.stateLocalRoster) {
        return Status::targetUnavailable;
    }
    return host::request_state_local_type2_sequence(view.binding,
                                                    prepared.target,
                                                    prepared.generatedRosterGroup,
                                                    sequence->keyHash,
                                                    prepared.activityClientGeneration,
                                                    &reservation)
               ? Status::queued
               : Status::refused;
}

/** An empty new-generation atom program cancels the current sequence. */
Status stop_combatant_sequence(const sdk::BoundView& view, std::uint32_t slotRow) noexcept {
    PreparedDevice prepared{};
    const Status status = prepare_combatant(view, slotRow, prepared);
    if (status != Status::ready) {
        return status;
    }
    if (!prepared.target.stateLocalRoster) {
        return Status::targetUnavailable;
    }
    return host::request_state_local_type2_sequence(view.binding,
                                                    prepared.target,
                                                    prepared.generatedRosterGroup,
                                                    0,
                                                    prepared.activityClientGeneration)
               ? Status::queued
               : Status::refused;
}

/**
 * Arms one combatant for its scene's squad member spawn.
 * @param view Current SDK and activity binding.
 * @param slotRow Exact combatant slot in that SDK.
 * @return Queued only when the binding request reaches the host.
 */
Status bind_combatant_to_squad(const sdk::BoundView& view, std::uint32_t slotRow) noexcept {
    PreparedDevice prepared{};
    const Status status = prepare_combatant(view, slotRow, prepared);
    if (status != Status::ready) {
        return status;
    }
    if (!prepared.target.stateLocalRoster) {
        return Status::targetUnavailable;
    }
    return host::request_state_local_type2_squad_binding(view.binding,
                                                         prepared.target,
                                                         prepared.generatedRosterGroup,
                                                         prepared.activityClientGeneration)
               ? Status::queued
               : Status::refused;
}

/** Resolves and queues squad binding for one exact generated combatant slot. */
Status
bind_combatant_to_squad_reserved(const sdk::BoundView& view,
                                 std::uint32_t slotRow,
                                 const host::ScriptableOutputReservation& reservation) noexcept {
    PreparedDevice prepared{};
    const Status status = prepare_combatant(view, slotRow, prepared);
    if (status != Status::ready) {
        return status;
    }
    if (!prepared.target.stateLocalRoster) {
        return Status::targetUnavailable;
    }
    return host::request_state_local_type2_squad_binding(view.binding,
                                                         prepared.target,
                                                         prepared.generatedRosterGroup,
                                                         prepared.activityClientGeneration,
                                                         &reservation)
               ? Status::queued
               : Status::refused;
}

Status availability(const sdk::BoundView& view,
                    std::uint32_t slotRow,
                    auth::Type23Channel channel,
                    float value,
                    bool snap) noexcept {
    (void)snap;
    PreparedDevice prepared{};
    return prepare(view, slotRow, channel, value, prepared);
}

/** Drives one device channel to a value, snapping instead of easing when asked. */
Status set_channel(const sdk::BoundView& view,
                   std::uint32_t slotRow,
                   auth::Type23Channel channel,
                   float value,
                   bool snap) noexcept {
    PreparedDevice prepared{};
    const Status status = prepare(view, slotRow, channel, value, prepared);
    if (status != Status::ready) {
        return status;
    }
    const bool queued =
        prepared.target.stateLocalRoster
            ? server::bap::request_activity_state_local_type23_override(
                  view.binding,
                  prepared.target,
                  prepared.generatedRosterGroup,
                  channel,
                  value,
                  snap,
                  prepared.effectiveRegion,
                  prepared.activityClientGeneration)
            : server::bap::request_activity_type23_override(view.binding,
                                                            prepared.target,
                                                            channel,
                                                            value,
                                                            snap,
                                                            prepared.effectiveRegion,
                                                            prepared.activityClientGeneration);
    if (queued) {
        return Status::queued;
    }
    return Status::refused;
}

/** Queues one preflighted device only through an exact unarmed Host revision. */
Status set_channel_reserved(const sdk::BoundView& view,
                            std::uint32_t slotRow,
                            auth::Type23Channel channel,
                            float value,
                            bool snap,
                            const host::ScriptableOutputReservation& reservation) noexcept {
    PreparedDevice prepared{};
    const Status status = prepare(view, slotRow, channel, value, prepared);
    if (status != Status::ready) {
        return status;
    }
    const bool queued =
        prepared.target.stateLocalRoster
            ? server::bap::request_activity_state_local_type23_override(
                  view.binding,
                  prepared.target,
                  prepared.generatedRosterGroup,
                  channel,
                  value,
                  snap,
                  prepared.effectiveRegion,
                  prepared.activityClientGeneration,
                  &reservation)
            : server::bap::request_activity_type23_override(view.binding,
                                                            prepared.target,
                                                            channel,
                                                            value,
                                                            snap,
                                                            prepared.effectiveRegion,
                                                            prepared.activityClientGeneration,
                                                            &reservation);
    if (queued) {
        return Status::queued;
    }
    return Status::refused;
}

Status trigger_availability(const sdk::BoundView& view, std::uint32_t slotRow) noexcept {
    PreparedDevice prepared{};
    return prepare_trigger(view, slotRow, prepared);
}

/** Fires one device's configured trigger. */
Status fire_trigger(const sdk::BoundView& view, std::uint32_t slotRow) noexcept {
    PreparedDevice prepared{};
    const Status status = prepare_trigger(view, slotRow, prepared);
    if (status != Status::ready) {
        return status;
    }
    const bool queued = prepared.target.stateLocalRoster
                            ? server::bap::request_activity_state_local_type31_override(
                                  view.binding,
                                  prepared.target,
                                  prepared.generatedRosterGroup,
                                  prepared.effectiveRegion,
                                  prepared.activityClientGeneration)
                            : server::bap::request_activity_type31_override(
                                  view.binding, prepared.target, prepared.effectiveRegion);
    if (queued) {
        return Status::queued;
    }
    return Status::refused;
}

/** Fires one device's configured trigger against an already-reserved Host output revision. */
Status fire_trigger_reserved(const sdk::BoundView& view,
                             std::uint32_t slotRow,
                             const host::ScriptableOutputReservation& reservation) noexcept {
    PreparedDevice prepared{};
    const Status status = prepare_trigger(view, slotRow, prepared);
    if (status != Status::ready) {
        return status;
    }
    const bool queued =
        prepared.target.stateLocalRoster
            ? server::bap::request_activity_state_local_type31_override(
                  view.binding,
                  prepared.target,
                  prepared.generatedRosterGroup,
                  prepared.effectiveRegion,
                  prepared.activityClientGeneration,
                  &reservation)
            : server::bap::request_activity_type31_override(
                  view.binding, prepared.target, prepared.effectiveRegion, &reservation);
    if (queued) {
        return Status::queued;
    }
    return Status::refused;
}

/** @return The stable log name of one device runtime status. */
const char* status_name(Status status) noexcept {
    switch (status) {
    case Status::ready:
        return "ready";
    case Status::queued:
        return "queued";
    case Status::invalidView:
        return "invalid_view";
    case Status::staleBinding:
        return "stale_binding";
    case Status::staleActivityClient:
        return "stale_activity_client";
    case Status::invalidSlot:
        return "invalid_slot";
    case Status::wrongScenario:
        return "wrong_scenario";
    case Status::schemaJoinNotExact:
        return "schema_join_not_exact";
    case Status::wrongSdkBuild:
        return "wrong_sdk_build";
    case Status::invalidBody:
        return "invalid_body";
    case Status::invalidChannel:
        return "invalid_channel";
    case Status::invalidValue:
        return "invalid_value";
    case Status::noActivityLink:
        return "no_activity_link";
    case Status::targetUnavailable:
        return "target_unavailable";
    case Status::ambiguousTarget:
        return "ambiguous_target";
    case Status::outputBusy:
        return "output_busy";
    case Status::refused:
        return "refused";
    case Status::refusedSlotType:
        return "refused_slot_type";
    }
    return "unknown";
}

} // namespace sunrise::server::activity::activity_sdk_devices
