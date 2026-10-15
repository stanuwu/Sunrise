#include <string_view>

#include "mission_script_lua_event_internal.h"
#include "mission_script_lua_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {

/** Resolves a typed event directly from its native enum. */
[[nodiscard]] std::string_view event_kind_name(host::EventKind kind) noexcept {
    switch (kind) {
    case host::EventKind::fireteamState:
        return "fireteamState";
    case host::EventKind::senseUpdate:
        return "sensorSenseUpdated";
    case host::EventKind::clientStateChanged:
        return "clientStateChanged";
    case host::EventKind::regionChanged:
        return "regionChanged";
    case host::EventKind::incidentReceived:
        return "incidentReceived";
    case host::EventKind::timerElapsed:
        return "timerElapsed";
    case host::EventKind::effectResult:
        return "effectResult";
    case host::EventKind::phaseEntered:
        return "phaseEntered";
    case host::EventKind::triggerEntered:
        return "triggerEntered";
    case host::EventKind::triggerExited:
        return "triggerExited";
    case host::EventKind::triggerState:
        return "triggerState";
    case host::EventKind::squadState:
        return "squadState";
    case host::EventKind::squadProvoked:
        return "squadProvoked";
    case host::EventKind::entitySpawned:
        return "entitySpawned";
    case host::EventKind::entityDied:
        return "entityDied";
    case host::EventKind::sceneFinished:
        return "sceneFinished";
    case host::EventKind::sceneOutput:
        return "sceneOutput";
    case host::EventKind::objectiveProgress:
        return "objectiveProgress";
    case host::EventKind::entitySlotsRequested:
        return "entitySlotsRequested";
    case host::EventKind::sessionJoined:
        return "sessionJoined";
    case host::EventKind::sessionLeft:
        return "sessionLeft";
    case host::EventKind::playerTrigger:
        return "playerTrigger";
    case host::EventKind::cinematicStarted:
        return "cinematicStarted";
    case host::EventKind::cinematicSkipRequested:
        return "cinematicSkipRequested";
    case host::EventKind::cinematicTerminated:
        return "cinematicTerminated";
    case host::EventKind::actorPathState:
        return "actorPathState";
    case host::EventKind::objectInteracted:
        return "objectInteracted";
    case host::EventKind::damageState:
        return "damageState";
    case host::EventKind::deviceState:
        return "deviceState";
    case host::EventKind::objectState:
        return "objectState";
    case host::EventKind::ghostLinkState:
        return "ghostLinkState";
    case host::EventKind::clientMessageReceived:
        return "clientMessageReceived";
    case host::EventKind::authStateCommitted:
        return "authStateCommitted";
    case host::EventKind::authStateTransportStaged:
        return "authStateTransportStaged";
    case host::EventKind::authStateCanceled:
        return "authStateCanceled";
    case host::EventKind::incidentQueued:
        return "incidentQueued";
    case host::EventKind::incidentTransportStaged:
        return "incidentTransportStaged";
    case host::EventKind::incidentCanceled:
        return "incidentCanceled";
    case host::EventKind::incidentRefused:
        return "incidentRefused";
    case host::EventKind::scriptableOverrideCommitted:
        return "scriptableOverrideCommitted";
    case host::EventKind::scriptableOverrideTransportStaged:
        return "scriptableOverrideTransportStaged";
    case host::EventKind::scriptableOverrideCanceled:
        return "scriptableOverrideCanceled";
    case host::EventKind::operatorRefused:
        return "operatorRefused";
    }
    return "unknown";
}

/**
 * Reads one event argument of any view.
 * @param index Stack index of the argument.
 * @return The event. The argument must be an event view.
 */
[[nodiscard]] const host::Event& check_event(lua_State* state, int index) {
    const void* const handle = luaL_testudata(state, index, kEventMetatable);
    if (handle == nullptr) {
        raise_lua_error(state, "mission event expected");
    }
    return static_cast<const EventHandle*>(handle)->value;
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
