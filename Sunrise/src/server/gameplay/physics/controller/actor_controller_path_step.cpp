#include <algorithm>
#include <cstddef>

#include "actor_controller_service.h"
#include "controller_internal.h"

namespace sunrise::server::gameplay::physics::controller {

using namespace internal;

/**
 * Runs facing, path, arrival, and stuck mechanics for one validated actor.
 * @param slotIndex Exact occupied controller slot.
 * @param slot Mutable generic controller state.
 * @param body Live kinematic body copied before this tick.
 * @param facingActor Exact facing target when the profile requires one.
 * @param tick Fixed tick which owns any outcome.
 * @param result Receives bounded work counts.
 */
void ActorControllerService::step_controller(std::size_t slotIndex,
                                             Slot& slot,
                                             const backend::BodyState& body,
                                             const world::ActorSnapshot* facingActor,
                                             world::TickId tick,
                                             ControllerStepResult& result) noexcept {
    const backend::Vec3 position = body.spec.transform.position;
    if (slot.state != ControllerState::followingPath) {
        if ((slot.state != ControllerState::idle && slot.state != ControllerState::arrived)
            || slot.profile.facing == FacingMode::none) {
            return;
        }
        const backend::Vec3 direction = facing_direction(slot.profile.facing,
                                                         slot.commandedVelocity,
                                                         slot.facingPosition,
                                                         position,
                                                         facingActor);
        if (length(direction) == 0.0F) {
            return;
        }
        backend::Transform target = body.spec.transform;
        target.rotation = bounded_facing(
            target.rotation, direction, slot.profile.maxTurnRadiansPerSecond * fixedDt_);
        if (!physics_->set_kinematic_target(slot.binding.body, target)) {
            stop_with_state(slot, ControllerState::contextMismatch);
            static_cast<void>(append_event(slotIndex,
                                           tick,
                                           ControllerEventKind::contextMismatch,
                                           ControllerStatus::contextMismatch,
                                           backend::NavigationStatus::contextMismatch,
                                           to_world(position)));
            return;
        }
        ++result.targetsQueued;
        ++slot.revision;
        return;
    }
    if (slot.path.count == 0 || slot.path.count > slot.path.points.size()
        || slot.waypointIndex > slot.path.count) {
        stop_with_state(slot, ControllerState::pathFailed);
        static_cast<void>(append_event(slotIndex,
                                       tick,
                                       ControllerEventKind::pathFailed,
                                       ControllerStatus::navigationFailed,
                                       backend::NavigationStatus::invalidQuery,
                                       to_world(position)));
        return;
    }

    backend::ProjectPointQuery projection{};
    projection.point = {slot.binding.navigationScene, slot.binding.bubble, position};
    projection.agent = slot.binding.navigationAgent;
    backend::NavigationPoint projected{};
    const backend::NavigationStatus projectionStatus =
        navigation_ == nullptr ? backend::NavigationStatus::staleScene
                               : navigation_->project_point(projection, projected);
    if (projectionStatus != backend::NavigationStatus::success) {
        stop_with_state(slot, ControllerState::pathFailed);
        static_cast<void>(append_event(slotIndex,
                                       tick,
                                       ControllerEventKind::pathFailed,
                                       ControllerStatus::navigationFailed,
                                       projectionStatus,
                                       to_world(position)));
        return;
    }

    while (slot.waypointIndex < slot.path.count) {
        const float waypointDistance =
            distance(position, slot.path.points[slot.waypointIndex].position);
        const bool finalWaypoint = slot.waypointIndex + 1U == slot.path.count;
        const float radius =
            finalWaypoint ? slot.profile.arrivalRadius : slot.profile.waypointRadius;
        if (waypointDistance > radius) {
            break;
        }
        ++slot.waypointIndex;
        slot.hasProgressSample = false;
        slot.stalledTicks = 0;
    }
    if (slot.waypointIndex >= slot.path.count) {
        stop_with_state(slot, ControllerState::arrived);
        static_cast<void>(append_event(slotIndex,
                                       tick,
                                       ControllerEventKind::pathComplete,
                                       ControllerStatus::success,
                                       backend::NavigationStatus::success,
                                       to_world(position)));
        return;
    }

    const backend::NavigationPoint& waypoint = slot.path.points[slot.waypointIndex];
    const float waypointDistance = distance(position, waypoint.position);
    if (!slot.hasProgressSample) {
        slot.lastWaypointDistance = waypointDistance;
        slot.stalledTicks = 0;
        slot.hasProgressSample = true;
    } else if (waypointDistance + slot.profile.progressEpsilon < slot.lastWaypointDistance) {
        slot.lastWaypointDistance = waypointDistance;
        slot.stalledTicks = 0;
    } else {
        ++slot.stalledTicks;
    }
    if (slot.stalledTicks >= slot.profile.stuckTickLimit) {
        const bool requestRepath = slot.profile.requestRepathWhenStuck;
        stop_with_state(slot, ControllerState::stuck);
        static_cast<void>(append_event(slotIndex,
                                       tick,
                                       ControllerEventKind::stuck,
                                       ControllerStatus::navigationFailed,
                                       backend::NavigationStatus::blocked,
                                       to_world(position)));
        if (requestRepath) {
            static_cast<void>(append_event(slotIndex,
                                           tick,
                                           ControllerEventKind::repathRequested,
                                           ControllerStatus::navigationFailed,
                                           backend::NavigationStatus::blocked,
                                           to_world(position)));
        }
        return;
    }

    backend::ReachabilityQuery reachability{};
    reachability.start = {slot.binding.navigationScene, slot.binding.bubble, position};
    reachability.goal = waypoint;
    reachability.agent = slot.binding.navigationAgent;
    const backend::NavigationStatus reachableStatus = navigation_->reachable(reachability);
    if (reachableStatus != backend::NavigationStatus::success) {
        stop_with_state(slot, ControllerState::pathFailed);
        static_cast<void>(append_event(slotIndex,
                                       tick,
                                       ControllerEventKind::pathFailed,
                                       ControllerStatus::navigationFailed,
                                       reachableStatus,
                                       to_world(position)));
        return;
    }

    const backend::Vec3 offset = subtract(waypoint.position, position);
    const float desiredSpeed = std::min(slot.profile.maxSpeed, waypointDistance / fixedDt_);
    const backend::Vec3 desiredVelocity = waypointDistance > 0.0F
                                              ? multiply(offset, desiredSpeed / waypointDistance)
                                              : backend::Vec3{};
    slot.commandedVelocity = bounded_velocity(slot.commandedVelocity,
                                              desiredVelocity,
                                              slot.profile.maxAcceleration * fixedDt_,
                                              slot.profile.maxSpeed);
    backend::Transform target = body.spec.transform;
    target.position = add(position, multiply(slot.commandedVelocity, fixedDt_));
    target.rotation = bounded_facing(target.rotation,
                                     facing_direction(slot.profile.facing,
                                                      slot.commandedVelocity,
                                                      slot.facingPosition,
                                                      position,
                                                      facingActor),
                                     slot.profile.maxTurnRadiansPerSecond * fixedDt_);
    if (!physics_->set_kinematic_target(slot.binding.body, target)) {
        stop_with_state(slot, ControllerState::pathFailed);
        static_cast<void>(append_event(slotIndex,
                                       tick,
                                       ControllerEventKind::pathFailed,
                                       ControllerStatus::navigationFailed,
                                       backend::NavigationStatus::invalidQuery,
                                       to_world(position)));
        return;
    }
    ++slot.revision;
    ++result.targetsQueued;
}

} // namespace sunrise::server::gameplay::physics::controller
