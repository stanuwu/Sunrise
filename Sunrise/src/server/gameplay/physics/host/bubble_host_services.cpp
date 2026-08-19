#include "bubble_host.h"
#include "internal.h"

namespace sunrise::server::gameplay::physics::host {
namespace {

[[nodiscard]] backend::Transform to_backend(const world::Transform& transform) noexcept {
    return {
        {transform.position.x, transform.position.y, transform.position.z},
        {transform.rotation.x, transform.rotation.y, transform.rotation.z, transform.rotation.w}};
}

} // namespace

/** Creates one tracked body for a live logical actor. */
HostStatus BubbleHost::bind_actor_body(WorldHandle handle,
                                       world::ActorKey actor,
                                       const backend::BodySpec& spec,
                                       backend::BodyHandle& body) noexcept {
    body = {};
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    world::WorldSnapshot snapshot{};
    if (!slot.runner.snapshot(snapshot)) {
        return HostStatus::unhealthy;
    }
    const world::ActorSnapshot* liveActor = nullptr;
    for (std::size_t index = 0; index < snapshot.actorCount; ++index) {
        if (world::equal_actor(snapshot.actors[index].key, actor)) {
            liveActor = &snapshot.actors[index];
            break;
        }
    }
    if (liveActor == nullptr || spec.scene != slot.physicsScene || spec.stableOwnerId != actor.id
        || spec.gate.bubble != liveActor->bubble
        || (spec.motion != backend::MotionKind::staticBody
            && liveActor->authority.mode == world::MotionAuthorityMode::clientPresentational)) {
        return HostStatus::invalidArgument;
    }
    for (const Storage::BodyBinding& binding : slot.bodies) {
        if (binding.occupied && world::equal_actor(binding.actor, actor)) {
            return HostStatus::alreadyOpen;
        }
    }

    backend::BodySpec canonical = spec;
    canonical.transform = to_backend(liveActor->transform);
    if (!slot.physics.create_body(canonical, body)) {
        return HostStatus::backendRejected;
    }
    for (Storage::BodyBinding& binding : slot.bodies) {
        if (!binding.occupied) {
            binding.actor = actor;
            binding.body = body;
            binding.occupied = true;
            return HostStatus::success;
        }
    }
    static_cast<void>(slot.physics.destroy_body(body));
    body = {};
    return HostStatus::capacity;
}

/** Destroys one exact tracked actor body. */
HostStatus BubbleHost::unbind_actor_body(WorldHandle handle, world::ActorKey actor) noexcept {
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    for (Storage::BodyBinding& binding : slot.bodies) {
        if (!binding.occupied || !world::equal_actor(binding.actor, actor)) {
            continue;
        }
        if (!slot.physics.destroy_body(binding.body)) {
            slot.healthy = false;
            return HostStatus::unhealthy;
        }
        binding = {};
        return HostStatus::success;
    }
    return HostStatus::staleHandle;
}

/** Copies one exact tracked actor body. */
HostStatus BubbleHost::read_actor_body(WorldHandle handle,
                                       world::ActorKey actor,
                                       backend::BodyState& body) const noexcept {
    body = {};
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    const Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    for (const Storage::BodyBinding& binding : slot.bodies) {
        if (binding.occupied && world::equal_actor(binding.actor, actor)) {
            return slot.physics.read_body(binding.body, body) ? HostStatus::success
                                                              : HostStatus::unhealthy;
        }
    }
    return HostStatus::staleHandle;
}

/** Runs one validated scriptless projection in the exact world. */
HostStatus BubbleHost::project_point(WorldHandle handle,
                                     const backend::ProjectPointQuery& query,
                                     backend::NavigationPoint& point,
                                     backend::NavigationStatus& status) const noexcept {
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        status = backend::NavigationStatus::staleScene;
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    status = storage_->worlds[slotIndex].navigation.project_point(query, point);
    return HostStatus::success;
}

/** Runs one bounded scriptless path query in the exact world. */
HostStatus BubbleHost::find_path(WorldHandle handle,
                                 const backend::ReachabilityQuery& query,
                                 backend::NavigationPath& path,
                                 backend::NavigationStatus& status) const noexcept {
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        path = {};
        status = backend::NavigationStatus::staleScene;
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    status = storage_->worlds[slotIndex].navigation.find_path(query, path);
    return HostStatus::success;
}

} // namespace sunrise::server::gameplay::physics::host
