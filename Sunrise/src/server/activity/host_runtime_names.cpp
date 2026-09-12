#include "../../middleware/bap/activity_message/wire_schema/activity_wire_schema.h"
#include "host_runtime.h"

namespace sunrise::server::activity::host {

/** @return Stable UI name for one event kind. */
const char* event_name(EventKind kind) noexcept {
    switch (kind) {
    case EventKind::senseUpdate:
        return "sensor state update";
    case EventKind::clientStateChanged:
        return "client state changed";
    case EventKind::clientMessageReceived:
        return "client message received";
    case EventKind::authStateCommitted:
        return "presentation state queued";
    case EventKind::authStateTransportStaged:
        return "presentation state staged";
    case EventKind::authStateCanceled:
        return "presentation state canceled";
    case EventKind::incidentReceived:
        return "incident received";
    case EventKind::incidentQueued:
        return "incident queued";
    case EventKind::incidentTransportStaged:
        return "incident staged";
    case EventKind::incidentCanceled:
        return "incident canceled";
    case EventKind::incidentRefused:
        return "incident refused";
    case EventKind::scriptableOverrideCommitted:
        return "scriptable action queued";
    case EventKind::scriptableOverrideTransportStaged:
        return "scriptable action staged";
    case EventKind::scriptableOverrideCanceled:
        return "scriptable action canceled";
    case EventKind::operatorRefused:
        return "operator refused";
    case EventKind::timerElapsed:
        return "mission timer elapsed";
    case EventKind::effectResult:
        return "script effect result";
    case EventKind::phaseEntered:
        return "mission phase entered";
    case EventKind::triggerEntered:
        return "trigger volume entered";
    case EventKind::triggerExited:
        return "trigger volume exited";
    case EventKind::squadState:
        return "squad state changed";
    case EventKind::squadProvoked:
        return "squad provoked";
    case EventKind::entitySpawned:
        return "squad slot count rose";
    case EventKind::entityDied:
        return "squad alive count fell";
    case EventKind::sceneFinished:
        return "authored scene finished";
    case EventKind::objectiveProgress:
        return "objective task counter rose";
    case EventKind::entitySlotsRequested:
        return "entity slots requested";
    case EventKind::sessionJoined:
        return "session joined";
    case EventKind::sessionLeft:
        return "session left";
    case EventKind::playerTrigger:
        return "player trigger";
    case EventKind::cinematicStarted:
        return "cinematic started";
    case EventKind::actorPathState:
        return "actor path state";
    case EventKind::fireteamState:
        return "fireteam state";
    case EventKind::damageState:
        return "damage state";
    case EventKind::deviceState:
        return "device state";
    case EventKind::objectState:
        return "object state";
    case EventKind::objectInteracted:
        return "object interacted";
    case EventKind::ghostLinkState:
        return "ghost link state";
    case EventKind::cinematicSkipRequested:
        return "cinematic skip requested";
    case EventKind::cinematicTerminated:
        return "cinematic terminated";
    }
    return "unknown";
}

/** @return Stable UI name for one framing verdict. */
const char* verdict_name(state::activity::receipts::Verdict verdict) noexcept {
    using Verdict = state::activity::receipts::Verdict;
    switch (verdict) {
    case Verdict::absent:
        return "absent";
    case Verdict::framed:
        return "framed";
    case Verdict::partial:
        return "partial";
    case Verdict::malformed:
        return "malformed";
    case Verdict::quarantined:
        return "quarantined";
    case Verdict::unowned:
        return "unowned";
    }
    return "unknown";
}

/** @return Stable UI name for one output attempt status. */
const char* output_status_name(OutputStatus status) noexcept {
    switch (status) {
    case OutputStatus::idle:
        return "idle";
    case OutputStatus::pending:
        return "pending";
    case OutputStatus::waitingForEpoch:
        return "waiting for epoch";
    case OutputStatus::noLayout:
        return "no roster layout";
    case OutputStatus::noGroups:
        return "no roster groups";
    case OutputStatus::noOverrideTarget:
        return "typed target is not registered here";
    case OutputStatus::ambiguousLinks:
        return "typed target has multiple activity links";
    case OutputStatus::frameRefused:
        return "frame/capacity refused";
    case OutputStatus::transportStaged:
        return "staged to transport";
    case OutputStatus::canceled:
        return "canceled: activity ended";
    }
    return "unknown";
}

/** @return Stable UI name for one retained incident status. */
const char* incident_status_name(IncidentStatus status) noexcept {
    switch (status) {
    case IncidentStatus::received:
        return "received";
    case IncidentStatus::queued:
        return "queued";
    case IncidentStatus::encodeFailed:
        return "encode failed";
    case IncidentStatus::frameRefused:
        return "frame/capacity refused";
    case IncidentStatus::transportStaged:
        return "staged to transport";
    case IncidentStatus::canceled:
        return "canceled: activity ended";
    }
    return "unknown";
}

/** @return Stable diagnostic name for one client message type. */
const char* client_message_name(std::uint32_t messageType) noexcept {
    const auto* const message =
        middleware::bap::activity_message::wire_schema::find_message(messageType);
    return message != nullptr ? message->name.data() : "unknown_activity_message";
}

/** @return Stable diagnostic name for one client body status. */
const char* client_message_status_name(ClientMessageStatus status) noexcept {
    switch (status) {
    case ClientMessageStatus::unclassified:
        return "unclassified";
    case ClientMessageStatus::decoded:
        return "decoded";
    case ClientMessageStatus::decodedPartial:
        return "decoded; selected values skipped";
    case ClientMessageStatus::prefixOnly:
        return "prefix only";
    case ClientMessageStatus::opaque:
        return "opaque body";
    case ClientMessageStatus::outerDecoded:
        return "outer decoded; target opaque";
    case ClientMessageStatus::prepared:
        return "accepted; parser depth not retained";
    case ClientMessageStatus::prepareRefused:
        return "decoded; State/transaction refused";
    case ClientMessageStatus::malformed:
        return "malformed";
    case ClientMessageStatus::quarantined:
        return "quarantined";
    }
    return "unknown";
}

} // namespace sunrise::server::activity::host
