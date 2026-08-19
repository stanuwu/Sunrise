#include <cstddef>
#include <cstdint>

#include "actor_controller_service.h"
#include "controller_internal.h"

namespace sunrise::server::gameplay::physics::controller {
namespace {

using namespace internal;

/** @param state Candidate stored phase. @return True for a defined phase. */
[[nodiscard]] bool valid_state(ControllerState state) noexcept {
    return state >= ControllerState::idle && state <= ControllerState::contextMismatch;
}

/**
 * Validates one pointer-free record against its live body binding.
 * @param snapshot Candidate controller state.
 * @param physics Exact backend used after restore.
 * @return True when identity, bounds, state, and path references agree.
 */
[[nodiscard]] bool valid_snapshot_controller(const ControllerSnapshot& snapshot,
                                             const backend::PhysicsBackend& physics) noexcept {
    if (!actor_valid(snapshot.binding.actor) || !valid_profile(snapshot.profile)
        || !finite(snapshot.facingPosition) || !finite(snapshot.commandedVelocity)
        || !finite(snapshot.lastWaypointDistance) || snapshot.lastWaypointDistance < 0.0F
        || !valid_state(snapshot.state) || snapshot.path.count > snapshot.path.points.size()
        || snapshot.waypointIndex > snapshot.path.count) {
        return false;
    }
    if (snapshot.profile.facing == FacingMode::actor
        && (!actor_valid(snapshot.targetActor)
            || actor_equal(snapshot.targetActor, snapshot.binding.actor))) {
        return false;
    }
    if (snapshot.state == ControllerState::followingPath
        && (snapshot.path.count == 0 || snapshot.waypointIndex >= snapshot.path.count)) {
        return false;
    }
    backend::BodyState body{};
    if (!physics.read_body(snapshot.binding.body, body)
        || body.spec.stableOwnerId != snapshot.binding.actor.id
        || body.spec.motion != backend::MotionKind::kinematic
        || body.spec.gate.bubble != snapshot.binding.bubble
        || snapshot.binding.navigationAgent.stableOwnerId != snapshot.binding.actor.id
        || snapshot.binding.navigationAgent.ignoreOwnerId != snapshot.binding.actor.id
        || snapshot.binding.navigationAgent.shapeId == 0
        || snapshot.binding.navigationAgent.gate.bubble != snapshot.binding.bubble
        || snapshot.binding.navigationAgent.gate.layer >= backend::kCollisionLayerCount) {
        return false;
    }
    for (std::size_t index = 0; index < snapshot.path.count; ++index) {
        const backend::NavigationPoint& point = snapshot.path.points[index];
        if (point.scene != snapshot.binding.navigationScene
            || point.bubble != snapshot.binding.bubble || !finite(point.position)) {
            return false;
        }
    }
    return true;
}

} // namespace

/**
 * Copies generic controller state without outcomes or transport state.
 * @param snapshot Receives one immutable process-local checkpoint.
 * @return True when the initialized service is healthy.
 */
bool ActorControllerService::snapshot(ControllerServiceSnapshot& snapshot) const noexcept {
    if (!initialized_ || !healthy_) {
        return false;
    }
    snapshot = {};
    ControllerServiceSnapshot& next = snapshot;
    next.lastTick = lastTick_;
    next.nextEventSequence = nextEventSequence_;
    next.fixedDt = fixedDt_;
    next.version = kControllerSnapshotVersion;
    next.healthy = true;
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        const Slot& slot = slots_[index];
        if (!slot.occupied) {
            continue;
        }
        ControllerSnapshot& destination = next.controllers[next.controllerCount++];
        destination.handle = {static_cast<std::uint16_t>(index), slot.generation};
        destination.binding = slot.binding;
        destination.profile = slot.profile;
        destination.targetActor = slot.targetActor;
        destination.facingPosition = slot.facingPosition;
        destination.path = slot.path;
        destination.commandedVelocity = slot.commandedVelocity;
        destination.lastWaypointDistance = slot.lastWaypointDistance;
        destination.waypointIndex = slot.waypointIndex;
        destination.revision = slot.revision;
        destination.stalledTicks = slot.stalledTicks;
        destination.state = slot.state;
        destination.hasProgressSample = slot.hasProgressSample;
    }
    return true;
}

/**
 * Restores validated bindings while issuing fresh controller handles.
 * @param snapshot Pointer-free process-local checkpoint.
 * @return Success only after every record validates and commits.
 */
ControllerOperationResult
ActorControllerService::restore(const ControllerServiceSnapshot& snapshot) noexcept {
    if (!initialized_ || physics_ == nullptr || navigation_ == nullptr) {
        return {{}, backend::NavigationStatus::success, ControllerStatus::notInitialized};
    }
    if (snapshot.version != kControllerSnapshotVersion || !snapshot.healthy
        || snapshot.fixedDt != fixedDt_ || snapshot.controllerCount > slots_.size()
        || snapshot.nextEventSequence == 0) {
        return {{}, backend::NavigationStatus::success, ControllerStatus::invalid};
    }
    for (std::size_t index = 0; index < snapshot.controllerCount; ++index) {
        const ControllerSnapshot& candidate = snapshot.controllers[index];
        if (!valid_snapshot_controller(candidate, *physics_)) {
            return {{}, backend::NavigationStatus::success, ControllerStatus::invalid};
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (snapshot.controllers[earlier].binding.actor.id == candidate.binding.actor.id) {
                return {{}, backend::NavigationStatus::success, ControllerStatus::staleActor};
            }
        }
    }
    std::size_t reusableCount = 0;
    for (const Slot& slot : slots_) {
        const std::uint32_t next = next_generation(slot.generation);
        if (generation_reusable(next == 0 ? slot.generation : next)) {
            ++reusableCount;
        }
    }
    if (snapshot.controllerCount > reusableCount) {
        return {{}, backend::NavigationStatus::success, ControllerStatus::full};
    }

    for (Slot& slot : slots_) {
        if (slot.occupied) {
            static_cast<void>(stop_motion(slot));
        }
        const std::uint32_t next = next_generation(slot.generation);
        const std::uint32_t generation = next == 0 ? slot.generation : next;
        slot = {};
        slot.generation = generation;
    }
    controllerCount_ = 0;
    std::size_t destinationIndex = 0;
    for (std::size_t sourceIndex = 0; sourceIndex < snapshot.controllerCount; ++sourceIndex) {
        while (destinationIndex < slots_.size()
               && !generation_reusable(slots_[destinationIndex].generation)) {
            ++destinationIndex;
        }
        Slot& slot = slots_[destinationIndex++];
        const ControllerSnapshot& source = snapshot.controllers[sourceIndex];
        slot.binding = source.binding;
        slot.profile = source.profile;
        slot.targetActor = source.targetActor;
        slot.facingPosition = source.facingPosition;
        slot.path = source.path;
        slot.commandedVelocity = source.commandedVelocity;
        slot.lastWaypointDistance = source.lastWaypointDistance;
        slot.waypointIndex = source.waypointIndex;
        slot.revision = source.revision;
        slot.stalledTicks = source.stalledTicks;
        slot.state = source.state;
        slot.occupied = true;
        slot.hasProgressSample = source.hasProgressSample;
        static_cast<void>(stop_motion(slot));
        ++controllerCount_;
    }
    events_ = {};
    eventCount_ = 0;
    lastTick_ = snapshot.lastTick;
    nextEventSequence_ = snapshot.nextEventSequence;
    healthy_ = true;
    return {{}, backend::NavigationStatus::success, ControllerStatus::success};
}

} // namespace sunrise::server::gameplay::physics::controller
