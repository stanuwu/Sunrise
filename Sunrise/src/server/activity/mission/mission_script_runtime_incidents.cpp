#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>

#include "mission_script_cinematic.h"
#include "mission_script_player_trigger.h"
#include "mission_script_region.h"
#include "mission_script_runtime_internal.h"

// Message-19 incidents resolved to their authored source slots.

namespace sunrise::server::activity::mission {

/** Resolves a native player-trigger notification to its authored type-31 source. */
void push_player_trigger(RuntimeInstance& instance,
                         const host::Event& incident,
                         std::int32_t region) noexcept {
    if (!incident.hasPlayerTrigger) {
        return;
    }
    const state::build_data::scriptables::Snapshot* const world = instance.worldView.snapshot();
    if (world == nullptr) {
        return;
    }
    middleware::bap::activity_message::player_trigger_incident::Payload payload{};
    payload.registryKey = incident.playerTriggerRegistryKey;
    payload.slotType = incident.playerTriggerSlotType;
    payload.slotIndex = incident.playerTriggerSlotIndex;
    payload.resolvedObjectId = incident.playerTriggerResolvedObjectId;
    player_trigger::Source source{};
    const player_trigger::ResolveStatus status =
        player_trigger::resolve(*world, payload, source, region);
    if (status != player_trigger::ResolveStatus::ready) {
        // A public report handed to its owner mostly names free-roam volumes, so its miss is quiet.
        const bool handedAbsent =
            status == player_trigger::ResolveStatus::absent && region != kAnyRegion;
        log_line(handedAbsent ? core::log::Level::debug : core::log::Level::warn,
                 &instance,
                 "player_trigger",
                 status == player_trigger::ResolveStatus::ambiguous        ? "ambiguous"
                 : status == player_trigger::ResolveStatus::invalidCatalog ? "invalid_catalog"
                                                                           : "absent");
        return;
    }
    std::array<char, 128> fields{};
    const int written = std::snprintf(fields.data(),
                                      fields.size(),
                                      "object=%08x slot=%u volume_key=%08x volume_slot=%u",
                                      source.objectTag,
                                      static_cast<unsigned>(source.slotIndex),
                                      source.volumeRegistryKey,
                                      static_cast<unsigned>(source.volumeSlotIndex));
    log_line(core::log::Level::debug,
             &instance,
             "player_trigger",
             "resolved",
             written > 0 ? std::string_view(
                               fields.data(),
                               (std::min)(static_cast<std::size_t>(written), fields.size() - 1))
                         : std::string_view{});
    host::Event event = incident;
    event.kind = host::EventKind::playerTrigger;
    event.firstRegistryKey = source.registryKey;
    event.slotObjectTag = source.objectTag;
    event.firstSlotIndex = source.slotIndex;
    event.firstSlotType = source.slotType;
    event.slotSenseSchema = 0;
    event.playerTriggerRegistryKey = source.volumeRegistryKey;
    event.playerTriggerSlotType = static_cast<std::int8_t>(source.volumeSlotType);
    event.playerTriggerSlotIndex = static_cast<std::int16_t>(source.volumeSlotIndex);
    push_script_event(instance, event);
}

/** Forwards one integration report; the script decides how reports count. */
void push_integration(RuntimeInstance& instance, const host::Event& incident) noexcept {
    if (!incident.hasIntegration) {
        return;
    }
    host::Event event = incident;
    event.kind = host::EventKind::integration;
    push_script_event(instance, event);
}

/** Resolves a native cinematic notification to its exact authored Type-6 source. */
void push_cinematic(RuntimeInstance& instance, const host::Event& incident) noexcept {
    if (!incident.hasCinematic) {
        return;
    }
    const state::build_data::scriptables::Snapshot* const world = instance.worldView.snapshot();
    if (world == nullptr) {
        return;
    }
    middleware::bap::activity_message::cinematic_incident::Payload target{};
    target.registryKey = incident.cinematicRegistryKey;
    target.slotType = incident.cinematicSlotType;
    target.slotIndex = incident.cinematicSlotIndex;
    target.runtimeObjectId = incident.cinematicRuntimeObjectId;
    target.eventValue = incident.cinematicEventValue;
    cinematic::Source source{};
    const cinematic::ResolveStatus status = cinematic::resolve(*world, target, source);
    std::array<char, 128> fields{};
    const int written = std::snprintf(fields.data(),
                                      fields.size(),
                                      "registry=%08x slot_type=%d slot_index=%d target=%u",
                                      target.registryKey,
                                      static_cast<int>(target.slotType),
                                      static_cast<int>(target.slotIndex),
                                      incident.incidentTarget);
    const std::string_view detail =
        written > 0
            ? std::string_view(fields.data(),
                               (std::min)(static_cast<std::size_t>(written), fields.size() - 1))
            : std::string_view{};
    if (status != cinematic::ResolveStatus::ready) {
        log_line(core::log::Level::warn,
                 &instance,
                 "cinematic",
                 status == cinematic::ResolveStatus::ambiguous        ? "ambiguous"
                 : status == cinematic::ResolveStatus::invalidCatalog ? "invalid_catalog"
                                                                      : "absent",
                 detail);
        return;
    }
    using Signal = middleware::bap::activity_message::cinematic_incident::Signal;
    const Signal signal = incident.cinematicSignal;
    log_line(core::log::Level::debug,
             &instance,
             "cinematic",
             signal == Signal::started         ? "started"
             : signal == Signal::skipRequested ? "skip_requested"
                                               : "terminated",
             detail);
    host::Event event = incident;
    event.kind = signal == Signal::started         ? host::EventKind::cinematicStarted
                 : signal == Signal::skipRequested ? host::EventKind::cinematicSkipRequested
                                                   : host::EventKind::cinematicTerminated;
    event.firstRegistryKey = source.registryKey;
    event.slotObjectTag = source.objectTag;
    event.firstSlotIndex = source.slotIndex;
    event.firstSlotType = source.slotType;
    event.slotSenseSchema = 0;
    push_script_event(instance, event);
}

} // namespace sunrise::server::activity::mission
