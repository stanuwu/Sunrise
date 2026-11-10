#include <algorithm>
#include <cstdint>
#include <span>

#include "../../../middleware/bap/activity_message/scriptable_auth_body.h"
#include "mission_script_runtime_internal.h"

namespace sunrise::server::activity::mission {

/**
 * Builds the receipt of a transported carrier step that waits on its actor: the counters the Host
 * wrote and the accepted-input boundary its completion report must follow.
 * @return False when the staged body cannot be read.
 */
bool carrier_step_receipt(const RuntimeInstance& instance,
                          const lua_vm::Intent& intent,
                          mission_state::CarrierReceipt& output) noexcept {
    namespace auth = middleware::bap::activity_message::scriptable_auth;
    output = {};
    host::PendingScriptableOverride staged{};
    auth::Type2ProgramLayout layout{};
    if (!host::staged_scriptable_override(
            instance.view.binding, instance.expectedScriptableRevision, staged)
        || !staged.missionInputBoundaryKnown
        || !auth::inspect_type2_program(
            std::span(staged.body).first(staged.byteCount), staged.bitCount, layout)) {
        return false;
    }
    output.requestKey = intent.requestKey;
    output.sourceGeneration = staged.expectedActivityClientGeneration;
    output.inputSequenceAtStage = staged.missionInputSequenceAtStage;
    output.clientMessageSequenceAtStage = staged.clientMessageSequenceAtStage;
    output.objectTag = staged.target.objectTag;
    output.registryKey = staged.target.registryKey;
    output.slotIndex = staged.target.slotIndex;
    output.revision = static_cast<std::uint32_t>(staged.generation);
    output.actorGeneration = layout.spawnGeneration;
    output.laneCount = layout.laneCount;
    return true;
}

/**
 * Joins one changed actor level to the carrier step that waits on that actor. Only the link that
 * simulates the actor reports here, so the flight follows that link.
 */
void observe_carrier_actor(RuntimeInstance& instance,
                           const host::SenseObservation& observation,
                           const ActorPathLevel& level) noexcept {
    if (!instance.carriersActive || !instance.missionStateBound) {
        return;
    }
    mission_state::CarrierActorReport report{};
    report.attemptGeneration = instance.dispatchAttemptGeneration;
    report.sourceGeneration = observation.sourceGeneration;
    report.admitted = true;
    report.inputSequence = instance.dispatchInputSequence;
    report.clientMessageSequence = observation.clientMessageSequence;
    report.objectTag = observation.key.objectTag;
    report.registryKey = observation.key.registryKey;
    report.slotIndex = observation.key.slotIndex;
    report.actorGeneration = static_cast<std::uint32_t>(level.generation);
    report.programRevision = static_cast<std::uint32_t>(level.revision);
    report.programProgress = level.state;
    report.deliveryKnown = (level.seen & kActorSeenDelivery) == kActorSeenDelivery;
    report.deliveryRevision = static_cast<std::uint32_t>(level.deliveryRevision);
    report.deliveryState = level.deliveryState;
    report.suppressed = level.suppressed;
    mission_state::Snapshot snapshot{};
    bool changed = false;
    const auto status = mission_state::observe_carrier_actor(
        instance.view.binding, instance.programKey, report, snapshot, changed);
    if (status != mission_state::Status::ready) {
        fault_instance(instance, "accepted actor level could not join its carrier step");
        return;
    }
    if (changed) {
        accept_mission_state(instance, snapshot);
    }
}

/**
 * Moves carrier flights on while no delivery is in flight, so a queued step never races the head
 * being delivered, and raises carrier_state when a flight changed.
 */
void advance_carriers(RuntimeInstance& instance) noexcept {
    const auto live = live_links(instance);
    const auto advanced = std::span(instance.carrierLinks).first(instance.carrierLinkCount);
    if (!instance.carriersActive || instance.programStatus != ProgramStatus::loaded
        || instance.deliveryStage != DeliveryStage::idle
        || (instance.carrierRecordRevision == instance.missionRecordRevision
            && std::ranges::equal(live, advanced))) {
        return;
    }
    mission_state::Snapshot snapshot{};
    bool changed = false;
    const auto status = mission_state::advance_carriers(instance.view.binding,
                                                        instance.programKey,
                                                        live,
                                                        snapshot,
                                                        changed);
    if (status != mission_state::Status::ready) {
        fault_instance(instance, "carrier flights could not advance");
        return;
    }
    instance.carrierLinks = instance.liveLinks;
    instance.carrierLinkCount = instance.liveLinkCount;
    if (!changed) {
        instance.carrierRecordRevision = instance.missionRecordRevision;
        return;
    }
    if (!lua_vm::adopt_intents(
            instance.vm, snapshot.state.pendingIntents, snapshot.state.nextIntentKey)) {
        fault_instance(instance, "carrier steps could not join the program's queue");
        return;
    }
    accept_mission_state(instance, snapshot);
    instance.carrierRecordRevision = instance.missionRecordRevision;
    host::Event event = state_edge_event(instance);
    event.attemptGeneration = instance.attempt.generation;
    event.kind = host::EventKind::carrierState;
    push_script_event(instance, event);
}

} // namespace sunrise::server::activity::mission
