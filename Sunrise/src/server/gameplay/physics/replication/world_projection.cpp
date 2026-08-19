#include "world_projection.h"

namespace sunrise::server::gameplay::physics::replication {
namespace {

/** @return True when two transforms have the same canonical scalar representation. */
[[nodiscard]] bool equal_transform(const world::Transform& left,
                                   const world::Transform& right) noexcept {
    return left.position.x == right.position.x && left.position.y == right.position.y
           && left.position.z == right.position.z && left.rotation.x == right.rotation.x
           && left.rotation.y == right.rotation.y && left.rotation.z == right.rotation.z
           && left.rotation.w == right.rotation.w;
}

/** @return True when two authority records describe the same exact epoch. */
[[nodiscard]] bool equal_authority(const world::MotionAuthority& left,
                                   const world::MotionAuthority& right) noexcept {
    return left.ownerMemberId == right.ownerMemberId && left.generation == right.generation
           && left.validFromTick == right.validFromTick && left.mode == right.mode;
}

/** @return True when one canonical actor can enter the generic entity envelope. */
[[nodiscard]] bool valid_input(const world::WorldIdentity& worldIdentity,
                               const world::ActorSnapshot& actor,
                               const state::gameplay::physics::Allocation& allocation,
                               std::uint64_t combatRevision) noexcept {
    const bool parentValid =
        (actor.parent.id == 0 && actor.parent.generation == 0) || world::valid_actor(actor.parent);
    return world::valid_world(worldIdentity) && world::valid_actor(actor.key)
           && actor.policyOwnerId != 0 && world::valid_blueprint(actor.blueprint)
           && world::finite_transform(actor.transform) && actor.authority.generation != 0
           && actor.authority.validFromTick != 0
           && actor.authority.mode < world::MotionAuthorityMode::count && actor.revision != 0
           && actor.bubble <= 0xFFU && actor.lifecycle == world::ActorLifecycle::live && parentValid
           && !world::equal_actor(actor.parent, actor.key) && allocation.actorId == actor.key.id
           && allocation.generation != 0 && allocation.context.generation != 0
           && allocation.context.slot < state::gameplay::physics::kReplicaContextCapacity
           && allocation.token.index <= 0x1FFFU && allocation.token.incarnation <= 0x0FU
           && allocation.sequence >= 2 && combatRevision != 0;
}

/** Copies canonical actor fields into one immutable planner snapshot. */
void fill_output(const WorldProjectionState& state, ActorSnapshot& output) noexcept {
    output = {};
    output.allocation = state.allocation;
    output.versions = state.versions;
    output.transform.orientation = {state.actor.transform.rotation.x,
                                    state.actor.transform.rotation.y,
                                    state.actor.transform.rotation.z,
                                    state.actor.transform.rotation.w};
    output.transform.position = {state.actor.transform.position.x,
                                 state.actor.transform.position.y,
                                 state.actor.transform.position.z};
    output.parentActorId = state.actor.parent.id;
    output.authorityEpoch = state.actor.authority.generation;
    output.combatRevision = state.combatRevision;
    output.contentRevision = state.versions.content;
    output.bubble = state.actor.bubble;
}

/** @return True when every world field represented by the planner is unchanged. */
[[nodiscard]] bool equal_projected_actor(const world::ActorSnapshot& left,
                                         const world::ActorSnapshot& right) noexcept {
    return left.policyOwnerId == right.policyOwnerId && left.blueprint.id == right.blueprint.id
           && left.blueprint.version == right.blueprint.version
           && world::equal_actor(left.parent, right.parent)
           && equal_transform(left.transform, right.transform)
           && equal_authority(left.authority, right.authority) && left.bubble == right.bubble
           && left.lifecycle == right.lifecycle;
}

} // namespace

/** Projects one canonical actor without changing state on rejected input. */
ProjectionResult project_actor(const world::WorldIdentity& worldIdentity,
                               const world::ActorSnapshot& actor,
                               const state::gameplay::physics::Allocation& allocation,
                               std::uint64_t combatRevision,
                               WorldProjectionState& state,
                               ActorSnapshot& output) noexcept {
    if (!valid_input(worldIdentity, actor, allocation, combatRevision)) {
        return ProjectionResult::invalid;
    }
    WorldProjectionState candidate = state;
    if (!state.initialized) {
        candidate.world = worldIdentity;
        candidate.actor = actor;
        candidate.allocation = allocation;
        candidate.versions = {actor.revision,
                              actor.revision,
                              actor.revision,
                              combatRevision,
                              actor.revision,
                              actor.revision};
        candidate.combatRevision = combatRevision;
        candidate.initialized = true;
        state = candidate;
        fill_output(state, output);
        return ProjectionResult::created;
    }
    if (!world::equal_world(state.world, worldIdentity)
        || !world::equal_actor(state.actor.key, actor.key)
        || state.allocation.actorId != allocation.actorId
        || state.allocation.generation != allocation.generation
        || state.allocation.token.index != allocation.token.index
        || state.allocation.token.incarnation != allocation.token.incarnation
        || state.allocation.sequence != allocation.sequence || actor.revision < state.actor.revision
        || combatRevision < state.combatRevision
        || (actor.revision == state.actor.revision && !equal_projected_actor(state.actor, actor))) {
        return ProjectionResult::invalid;
    }

    bool changed = false;
    if (!equal_authority(state.actor.authority, actor.authority)) {
        candidate.versions.authority = actor.revision;
        changed = true;
    }
    if (!world::equal_actor(state.actor.parent, actor.parent)) {
        candidate.versions.parent = actor.revision;
        changed = true;
    }
    if (state.actor.policyOwnerId != actor.policyOwnerId
        || state.actor.blueprint.id != actor.blueprint.id
        || state.actor.blueprint.version != actor.blueprint.version
        || state.actor.bubble != actor.bubble) {
        candidate.versions.content = actor.revision;
        changed = true;
    }
    if (!equal_transform(state.actor.transform, actor.transform)) {
        candidate.versions.transform = actor.revision;
        changed = true;
    }
    if (combatRevision != state.combatRevision) {
        candidate.versions.combat = combatRevision;
        changed = true;
    }
    candidate.actor = actor;
    candidate.combatRevision = combatRevision;
    state = candidate;
    fill_output(state, output);
    return changed ? ProjectionResult::updated : ProjectionResult::unchanged;
}

} // namespace sunrise::server::gameplay::physics::replication
