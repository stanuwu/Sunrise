#include <algorithm>
#include <array>
#include <span>

#include "../../../state/activity/transactions/internal.h"
#include "mission_script_runtime_internal.h"

namespace sunrise::server::activity::mission {
namespace {

/** @return True for another committed member of this exact shared activity. */
[[nodiscard]] bool peer_session(const RuntimeInstance& instance,
                                const state::activity::SessionRosterRow& row) noexcept {
    std::uint64_t ownMember{};
    for (const auto& member : instance.memberInputs) {
        if (member.present
            && member.life.sourceGeneration == instance.view.activityClientGeneration) {
            ownMember = member.life.member.identity.memberKey;
        }
    }
    return row.joined && row.memberKey != ownMember
           && state::activity::same_binding(row.binding, instance.view.binding)
           && state::activity::transactions::same_destination(row.binding.destination,
                                                              instance.view.binding.destination);
}

/** Mirrors the watched roster into the VM so any callback can read the current peer set. */
void publish_peer_set(RuntimeInstance& instance) noexcept {
    std::array<lua_vm::PeerSession, state::activity::kSessionCapacity> peers{};
    std::size_t count = 0;
    for (const SessionRosterWatch& watched : instance.sessionRoster) {
        if (!watched.used) {
            continue;
        }
        peers[count] = {
            watched.sessionId, watched.createdRevision, watched.memberKey, watched.joinIdentity};
        ++count;
    }
    lua_vm::publish_peers(instance.vm, std::span(peers.data(), count));
}

} // namespace

/** @return The identity every host-state edge that is not a Sense edge carries. */
[[nodiscard]] host::Event state_edge_event(const RuntimeInstance& instance) noexcept {
    host::Event event{};
    event.binding = instance.view.binding;
    event.sequence = instance.missionStateRevision;
    event.sourceGeneration = instance.view.activityClientGeneration;
    event.missionSequence = instance.lastMissionSequence;
    return event;
}

/** Reports one committed phase change to the script. */
void queue_phase_entered(RuntimeInstance& instance, std::uint32_t previousPhase) noexcept {
    host::Event event = state_edge_event(instance);
    event.stateRevision = instance.missionStateRevision;
    event.missionPhase = instance.missionPhase;
    event.previousMissionPhase = previousPhase;
    event.kind = host::EventKind::phaseEntered;
    push_script_event(instance, event);
}

/**
 * Raises one event per peer member that appeared or left this exact shared activity.
 * A join lifetime replacement counts as a leave and a join. The first read only records the
 * current peers, so a reattach never replays them.
 */
void push_session_roster_edges(RuntimeInstance& instance) noexcept {
    refresh_mission_members(instance);
    if (!instance.partySnapshotValid) {
        return;
    }
    std::array<state::activity::SessionRosterRow,
               state::activity::entity_slots::kMemberLeaseRowCount>
        rows{};
    std::size_t count{};
    for (const auto& member : instance.memberInputs) {
        if (!member.present) {
            continue;
        }
        auto& row = rows[count++];
        row.binding = instance.view.binding;
        row.memberKey = member.life.member.identity.memberKey;
        row.joinIdentity = member.life.member.identity.joinIdentity;
        row.joinedRevision = member.life.member.joinedRevision;
        row.joined = true;
    }
    const auto roster = std::span(rows).first(count);
    const bool first = !instance.sessionRosterObserved;
    instance.sessionRosterObserved = true;
    for (SessionRosterWatch& watched : instance.sessionRoster) {
        if (!watched.used) {
            continue;
        }
        const auto match = std::find_if(
            roster.begin(), roster.end(), [&instance, &watched](const auto& row) noexcept {
                return peer_session(instance, row) && row.binding.sessionId == watched.sessionId
                       && row.binding.createdRevision == watched.createdRevision
                       && row.memberKey == watched.memberKey
                       && row.joinedRevision == watched.joinedRevision;
            });
        if (match != roster.end()) {
            continue;
        }
        host::Event event = state_edge_event(instance);
        event.peerSessionId = watched.sessionId;
        event.peerSessionGeneration = watched.createdRevision;
        event.peerMemberKey = watched.memberKey;
        event.kind = host::EventKind::sessionLeft;
        watched = {};
        push_script_event(instance, event);
    }
    for (const state::activity::SessionRosterRow& row : roster) {
        if (!peer_session(instance, row)) {
            continue;
        }
        SessionRosterWatch* spare = nullptr;
        bool known = false;
        for (SessionRosterWatch& watched : instance.sessionRoster) {
            if (watched.used && watched.sessionId == row.binding.sessionId
                && watched.createdRevision == row.binding.createdRevision
                && watched.memberKey == row.memberKey
                && watched.joinedRevision == row.joinedRevision) {
                watched.memberKey = row.memberKey;
                watched.joinIdentity = row.joinIdentity;
                known = true;
                break;
            }
            if (!watched.used && spare == nullptr) {
                spare = &watched;
            }
        }
        if (known || spare == nullptr) {
            continue;
        }
        spare->sessionId = row.binding.sessionId;
        spare->createdRevision = row.binding.createdRevision;
        spare->memberKey = row.memberKey;
        spare->joinIdentity = row.joinIdentity;
        spare->joinedRevision = row.joinedRevision;
        spare->used = true;
        if (first) {
            continue;
        }
        host::Event event = state_edge_event(instance);
        event.peerSessionId = row.binding.sessionId;
        event.peerSessionGeneration = row.binding.createdRevision;
        event.peerMemberKey = row.memberKey;
        event.stateRevision = row.joinedRevision;
        event.kind = host::EventKind::sessionJoined;
        push_script_event(instance, event);
    }
    publish_peer_set(instance);
}

} // namespace sunrise::server::activity::mission
