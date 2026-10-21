#pragma once
#include "../../bap/runtime.h"
#include "../host_runtime.h"
#include "mission_script_runtime_internal.h"

namespace sunrise::server::activity::mission {
/** Member-owned input is distinct from the selected route's shared simulation/output feed. */
inline bool member_mission_event(host::EventKind kind) noexcept {
    switch (kind) {
    case host::EventKind::senseUpdate:
    case host::EventKind::incidentReceived:
    case host::EventKind::clientStateChanged:
    case host::EventKind::regionChanged:
    case host::EventKind::clientMessageReceived:
    case host::EventKind::entitySlotsRequested:
    case host::EventKind::playerTrigger:
    case host::EventKind::triggerEntered:
    case host::EventKind::triggerExited:
    case host::EventKind::objectInteracted:
    case host::EventKind::ghostLinkState:
    case host::EventKind::cinematicStarted:
    case host::EventKind::cinematicSkipRequested:
    case host::EventKind::cinematicTerminated:
        return true;
    default:
        return false;
    }
}
/**
 * Applies the mission input policy to one host event.
 * Timers always pass, the selected route's own events pass, and member-owned input passes only
 * from a current member's live connection.
 * @param kind Host event kind.
 * @param selectedSource True when the event came from the program's selected, joined link.
 * @param currentMember True when the event came from a current member's live connection.
 * @return True when the event may reach the mission program.
 */
inline bool
mission_input_allowed(host::EventKind kind, bool selectedSource, bool currentMember) noexcept {
    return kind == host::EventKind::timerElapsed || selectedSource
           || (currentMember && member_mission_event(kind));
}
/** @return True for the host events that report an output's progress, not an input. */
[[nodiscard]] inline bool delivery_lifecycle_event(host::EventKind kind) noexcept {
    switch (kind) {
    case host::EventKind::authStateCommitted:
    case host::EventKind::authStateTransportStaged:
    case host::EventKind::authStateCanceled:
    case host::EventKind::incidentQueued:
    case host::EventKind::incidentTransportStaged:
    case host::EventKind::incidentCanceled:
    case host::EventKind::incidentRefused:
    case host::EventKind::scriptableOverrideCommitted:
    case host::EventKind::scriptableOverrideTransportStaged:
    case host::EventKind::scriptableOverrideCanceled:
    case host::EventKind::operatorRefused:
        return true;
    default:
        return false;
    }
}

/** True when the event may reach a callback for this instance's ActivityClient generation. */
[[nodiscard]] inline bool eligible_event(const RuntimeInstance& instance,
                                         const host::Event& event) noexcept {
    if (event.attemptGeneration != 0 && event.attemptGeneration != instance.attempt.generation) {
        return false;
    }
    if (event.kind == host::EventKind::timerElapsed) {
        return true;
    }
    bool currentMember = false;
    for (const auto& member : instance.memberInputs) {
        if (!member.present || member.life.sourceGeneration != event.sourceGeneration) {
            continue;
        }
        server::bap::ActivityLinkView live{};
        currentMember = server::bap::mission_member_link_view(
                            instance.view.binding, member.life.member.identity, live)
                        && live.activityClientGeneration == event.sourceGeneration;
        break;
    }
    // Entity damage keeps the generation of the entity source it was read from, because the
    // dispatch revalidates that source. The poll already admitted it from the link that simulates
    // its combatant, so any joined link of the binding may carry it.
    server::bap::ActivityLinkView selected{};
    const bool selectedSource =
        event.sourceGeneration != 0
        && (event.sourceGeneration == instance.view.activityClientGeneration
            || event.damageFromEntity)
        && server::bap::activity_link_view(instance.view.binding, event.sourceGeneration, selected)
        && selected.joined;
    if (!mission_input_allowed(event.kind, selectedSource, currentMember)) {
        return false;
    }
    return event.kind == host::EventKind::clientStateChanged
           || event.kind == host::EventKind::incidentReceived
           || event.kind == host::EventKind::clientMessageReceived
           || event.kind == host::EventKind::effectResult
           || event.kind == host::EventKind::phaseEntered
           || event.kind == host::EventKind::triggerEntered
           || event.kind == host::EventKind::triggerExited
           || event.kind == host::EventKind::squadState
           || event.kind == host::EventKind::squadProvoked
           || event.kind == host::EventKind::entitySpawned
           || event.kind == host::EventKind::entityDied
           || event.kind == host::EventKind::sceneFinished
           || event.kind == host::EventKind::sceneOutput
           || event.kind == host::EventKind::objectiveProgress
           || event.kind == host::EventKind::entitySlotsRequested
           || event.kind == host::EventKind::sessionJoined
           || event.kind == host::EventKind::sessionLeft
           || event.kind == host::EventKind::playerTrigger
           || event.kind == host::EventKind::cinematicStarted
           || event.kind == host::EventKind::actorPathState
           || event.kind == host::EventKind::damageState
           || event.kind == host::EventKind::regionChanged
           || event.kind == host::EventKind::deviceState
           || event.kind == host::EventKind::objectState
           || event.kind == host::EventKind::fireteamState
           || event.kind == host::EventKind::objectInteracted
           || event.kind == host::EventKind::ghostLinkState
           || event.kind == host::EventKind::cinematicSkipRequested
           || event.kind == host::EventKind::cinematicTerminated
           || delivery_lifecycle_event(event.kind) || event.has_sense_observations();
}

} // namespace sunrise::server::activity::mission
