#include "tick_runtime.h"

namespace sunrise::server::gameplay::physics::host::detail {
namespace {

[[nodiscard]] const world::ActorSnapshot* find_actor(std::span<const world::ActorSnapshot> actors,
                                                     world::ActorKey actor) noexcept {
    for (const world::ActorSnapshot& candidate : actors) {
        if (world::equal_actor(candidate.key, actor)) {
            return &candidate;
        }
    }
    return nullptr;
}

[[nodiscard]] backend::Transform to_backend(const world::Transform& transform) noexcept {
    return {
        {transform.position.x, transform.position.y, transform.position.z},
        {transform.rotation.x, transform.rotation.y, transform.rotation.z, transform.rotation.w}};
}

[[nodiscard]] world::Transform to_world(const backend::Transform& transform) noexcept {
    return {
        {transform.position.x, transform.position.y, transform.position.z},
        {transform.rotation.x, transform.rotation.y, transform.rotation.z, transform.rotation.w}};
}

[[nodiscard]] bool equal_transform(const world::Transform& left,
                                   const world::Transform& right) noexcept {
    return left.position.x == right.position.x && left.position.y == right.position.y
           && left.position.z == right.position.z && left.rotation.x == right.rotation.x
           && left.rotation.y == right.rotation.y && left.rotation.z == right.rotation.z
           && left.rotation.w == right.rotation.w;
}

} // namespace

/** Reconciles logical actors into bound non-dynamic backend bodies. */
bool sync_actor_bodies(BubbleHost::Storage::WorldSlot& slot,
                       const world::HostExecutionContext& context,
                       std::span<const world::ActorSnapshot> actors,
                       std::span<const world::CommittedEvent> events) noexcept {
    static_cast<void>(context);
    for (BubbleHost::Storage::BodyBinding& binding : slot.bodies) {
        if (!binding.occupied) {
            continue;
        }
        const world::ActorSnapshot* actor = find_actor(actors, binding.actor);
        if (actor == nullptr) {
            if (!slot.physics.destroy_body(binding.body)) {
                return false;
            }
            binding = {};
            continue;
        }
        backend::BodyState body{};
        if (!slot.physics.read_body(binding.body, body)) {
            return false;
        }
    }

    for (const world::CommittedEvent& event : events) {
        if (event.kind == world::CommittedEventKind::actorRemoved) {
            authority::MotionBaseline baseline{};
            if (slot.motion.query(event.actor, baseline) && !slot.motion.remove(event.actor)) {
                return false;
            }
            continue;
        }
        if (event.kind != world::CommittedEventKind::kinematicTargetChanged
            && event.kind != world::CommittedEventKind::actorTeleported) {
            continue;
        }
        for (BubbleHost::Storage::BodyBinding& binding : slot.bodies) {
            if (!binding.occupied || !world::equal_actor(binding.actor, event.actor)) {
                continue;
            }
            const backend::Transform target = to_backend(event.transform);
            const bool applied = event.kind == world::CommittedEventKind::kinematicTargetChanged
                                     ? slot.physics.set_kinematic_target(binding.body, target)
                                     : slot.physics.teleport_body(binding.body, target);
            if (!applied) {
                return false;
            }
            break;
        }
    }

    for (BubbleHost::Storage::CombatantBinding& binding : slot.combatants) {
        if (!binding.occupied || find_actor(actors, binding.actor) != nullptr) {
            continue;
        }
        if (slot.combat.remove_combatant(binding.combatant) != mechanics::CombatResult::applied) {
            return false;
        }
        binding = {};
    }
    for (BubbleHost::Storage::PathBinding& path : slot.paths) {
        if (path.occupied && find_actor(actors, path.actor) == nullptr) {
            path = {};
        }
    }
    return true;
}

/** Advances bound backend bodies once and retains the ordered contacts. */
bool run_physics(BubbleHost::Storage::WorldSlot& slot,
                 const world::HostExecutionContext& context) noexcept {
    if (context.fixedDeltaDenominator == 0) {
        return false;
    }
    const float fixedDelta = static_cast<float>(context.fixedDeltaNumerator)
                             / static_cast<float>(context.fixedDeltaDenominator);
    if (!slot.physics.step(fixedDelta)) {
        return false;
    }
    slot.contacts = {};
    return slot.physics.drain_contacts(slot.contacts);
}

/** Collects changed movable body poses for the runner's atomic canonical commit. */
bool collect_pose_commits(BubbleHost::Storage::WorldSlot& slot,
                          std::span<const world::ActorSnapshot> actors) noexcept {
    slot.poseCommits = {};
    slot.poseCommitCount = 0;
    for (const BubbleHost::Storage::BodyBinding& binding : slot.bodies) {
        if (!binding.occupied) {
            continue;
        }
        const world::ActorSnapshot* actor = find_actor(actors, binding.actor);
        backend::BodyState body{};
        if (actor == nullptr || !slot.physics.read_body(binding.body, body)) {
            return false;
        }
        if (body.spec.motion == backend::MotionKind::staticBody) {
            continue;
        }
        const world::Transform transform = to_world(body.spec.transform);
        if (equal_transform(transform, actor->transform)) {
            continue;
        }
        if (slot.poseCommitCount == slot.poseCommits.size()) {
            return false;
        }

        world::CommandSource source{};
        if (actor->authority.mode == world::MotionAuthorityMode::host) {
            source = {slot.runner.identity().ownerGeneration, world::CommandSourceKind::host};
        } else if (actor->authority.mode
                   == world::MotionAuthorityMode::clientPredictedHostChecked) {
            source = {actor->authority.ownerMemberId, world::CommandSourceKind::network};
        } else {
            return false;
        }
        world::CanonicalPoseCommit& commit = slot.poseCommits[slot.poseCommitCount++];
        commit.actor = actor->key;
        commit.transform = transform;
        commit.source = source;
        commit.policyOwnerId = actor->policyOwnerId;
        commit.authorityGeneration = actor->authority.generation;
    }
    return true;
}

} // namespace sunrise::server::gameplay::physics::host::detail
