#include <Windows.h>

#include <cstddef>
#include <cstdint>

#include "internal.h"
#include "runtime.h"

namespace sunrise::server::gameplay::physics::replication {

/** Attaches one prepared plan to an exact State ticket and packet generation. */
bool commit_plan(PeerState& peer,
                 const Plan& plan,
                 std::uint64_t packetGeneration,
                 ContributionHandle& contribution) noexcept {
    contribution = {};
    contribution.slot = kInFlightCapacity;
    ActorRecord* actor = detail::resolve_actor(peer, plan.actorSlot, plan.recordGeneration);
    if (actor == nullptr || plan.peerGeneration != peer.generation || packetGeneration == 0
        || actor->contributionPending
        || !detail::equal_allocation(actor->desired.allocation, plan.actor.allocation)
        || !detail::equal_versions(actor->desired.versions, plan.actor.versions)
        || !detail::equal_selected_payload(actor->desired, plan.actor, plan.fields)
        || detail::pending_operation(*actor) != plan.operation
        || !detail::causal_ready(peer, *actor, plan.operation)) {
        return false;
    }
    const FieldMask currentFields =
        plan.operation == Operation::create ? kCreateFieldMask : detail::dirty_fields(*actor);
    if (currentFields != plan.fields || plan.operation == Operation::none) {
        return false;
    }
    std::size_t slot = peer.inFlight.size();
    for (std::size_t index = 0; index < peer.inFlight.size(); ++index) {
        if (!peer.inFlight[index].occupied) {
            slot = index;
            break;
        }
    }
    std::uint64_t generation = 0;
    state::gameplay::physics::ContributionTicket ticket{};
    if (slot >= peer.inFlight.size()
        || !detail::claim_generation(peer.nextContributionGeneration, generation)
        || !state::gameplay::physics::open_contribution_ticket(
            peer.replica,
            actor->desired.allocation,
            state::gameplay::physics::ContributionKind::packet,
            ticket)) {
        return false;
    }
    InFlightRecord& stored = peer.inFlight[slot];
    stored.ticket = ticket;
    stored.actor = plan.actor;
    stored.generation = generation;
    stored.packetGeneration = packetGeneration;
    stored.actorRecordGeneration = actor->generation;
    stored.fields = plan.fields;
    stored.operation = plan.operation;
    stored.actorSlot = plan.actorSlot;
    stored.occupied = true;
    actor->contributionPending = true;
    contribution.peerGeneration = peer.generation;
    contribution.generation = generation;
    contribution.packetGeneration = packetGeneration;
    contribution.slot = slot;
    return true;
}

/** Settles one exact contribution without clearing newer desired state. */
bool settle(PeerState& peer,
            ContributionHandle contribution,
            std::uint64_t packetGeneration,
            Outcome outcome) noexcept {
    InFlightRecord* stored = detail::resolve_contribution(peer, contribution);
    if (stored == nullptr || packetGeneration == 0
        || contribution.packetGeneration != packetGeneration
        || stored->packetGeneration != packetGeneration
        || (outcome != Outcome::success && outcome != Outcome::lost)) {
        return false;
    }
    const InFlightRecord completed = *stored;
    ActorRecord* actor =
        detail::resolve_actor(peer, completed.actorSlot, completed.actorRecordGeneration);
    if (!state::gameplay::physics::settle_contribution_ticket(completed.ticket)) {
        return false;
    }
    SecureZeroMemory(stored, sizeof *stored);
    if (actor == nullptr) {
        return true;
    }
    actor->contributionPending = false;
    if (outcome == Outcome::lost) {
        if (completed.operation == Operation::create && actor->removeRequested) {
            const bool released =
                state::gameplay::physics::release_for_peer(peer.replica, actor->desired.allocation);
            SecureZeroMemory(actor, sizeof *actor);
            return released;
        }
        return true;
    }
    if (completed.operation == Operation::create) {
        actor->createCommitted = true;
        actor->baseline = completed.actor.versions;
        actor->baselineParentActorId = completed.actor.parentActorId;
        actor->baselineStreamSourceActorId = completed.actor.streamSourceActorId;
        return true;
    }
    if (completed.operation == Operation::update) {
        for (std::uint8_t value = 0; value < static_cast<std::uint8_t>(Field::count); ++value) {
            const auto field = static_cast<Field>(value);
            if ((completed.fields & field_bit(field)) != 0) {
                detail::set_field_version(
                    actor->baseline, field, detail::field_version(completed.actor.versions, field));
            }
        }
        if ((completed.fields & field_bit(Field::parent)) != 0) {
            actor->baselineParentActorId = completed.actor.parentActorId;
            actor->baselineStreamSourceActorId = completed.actor.streamSourceActorId;
        }
        return true;
    }
    if (completed.operation != Operation::remove) {
        return false;
    }
    const bool released =
        state::gameplay::physics::release_for_peer(peer.replica, actor->desired.allocation);
    SecureZeroMemory(actor, sizeof *actor);
    return released;
}

/** Counts actors still owned by one peer planner. */
std::size_t actor_count(const PeerState& peer) noexcept {
    std::size_t count = 0;
    for (const ActorRecord& actor : peer.actors) {
        count += actor.occupied ? 1U : 0U;
    }
    return count;
}

/** Counts external outcomes still waiting for a result. */
std::size_t contribution_count(const PeerState& peer) noexcept {
    std::size_t count = 0;
    for (const InFlightRecord& contribution : peer.inFlight) {
        count += contribution.occupied ? 1U : 0U;
    }
    return count;
}

} // namespace sunrise::server::gameplay::physics::replication
