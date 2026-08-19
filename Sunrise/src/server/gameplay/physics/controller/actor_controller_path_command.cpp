#include <cstddef>
#include <cstdint>

#include "actor_controller_service.h"
#include "controller_internal.h"

namespace sunrise::server::gameplay::physics::controller {

using namespace internal;

/**
 * Resolves and commits one bounded path for an exact actor.
 * @param command Typed path request from the host queue.
 * @param tick Fixed tick which owns any failure event.
 * @return Success or an exact path and validation failure.
 */
ControllerOperationResult
ActorControllerService::request_path(const world::RequestPathCommand& command,
                                     world::TickId tick) noexcept {
    if (!initialized_ || physics_ == nullptr || navigation_ == nullptr) {
        return {{}, backend::NavigationStatus::success, ControllerStatus::notInitialized};
    }
    if (!healthy_) {
        return {{}, backend::NavigationStatus::success, ControllerStatus::unhealthy};
    }
    if (tick == 0 || tick < lastTick_ || !actor_valid(command.actor)
        || !blueprint_valid(command.navigationProfile) || !finite(command.destination)) {
        const ControllerStatus status =
            tick != 0 && tick < lastTick_ ? ControllerStatus::late : ControllerStatus::invalid;
        return {{}, backend::NavigationStatus::success, status};
    }
    std::size_t index = slots_.size();
    bool reusedActorId = false;
    for (std::size_t candidate = 0; candidate < slots_.size(); ++candidate) {
        if (!slots_[candidate].occupied || slots_[candidate].binding.actor.id != command.actor.id) {
            continue;
        }
        if (actor_equal(slots_[candidate].binding.actor, command.actor)) {
            index = candidate;
        } else {
            reusedActorId = true;
        }
        break;
    }
    if (index == slots_.size()) {
        const ControllerStatus status =
            reusedActorId ? ControllerStatus::staleActor : ControllerStatus::notFound;
        return {{}, backend::NavigationStatus::success, status};
    }
    Slot& slot = slots_[index];
    const ControllerHandle handle{static_cast<std::uint16_t>(index), slot.generation};
    if (!blueprint_equal(command.navigationProfile, slot.profile.navigationProfile)) {
        return {handle, backend::NavigationStatus::success, ControllerStatus::profileMismatch};
    }
    backend::BodyState body{};
    if (!physics_->read_body(slot.binding.body, body)) {
        stop_with_state(slot, ControllerState::actorStale);
        if (!append_event(index,
                          tick,
                          ControllerEventKind::actorStale,
                          ControllerStatus::staleBody,
                          backend::NavigationStatus::staleScene,
                          {})) {
            return {handle, backend::NavigationStatus::staleScene, ControllerStatus::unhealthy};
        }
        return {handle, backend::NavigationStatus::staleScene, ControllerStatus::staleBody};
    }
    if (body.spec.stableOwnerId != slot.binding.actor.id
        || body.spec.gate.bubble != slot.binding.bubble) {
        stop_with_state(slot, ControllerState::contextMismatch);
        if (!append_event(index,
                          tick,
                          ControllerEventKind::contextMismatch,
                          ControllerStatus::contextMismatch,
                          backend::NavigationStatus::contextMismatch,
                          to_world(body.spec.transform.position))) {
            return {
                handle, backend::NavigationStatus::contextMismatch, ControllerStatus::unhealthy};
        }
        return {
            handle, backend::NavigationStatus::contextMismatch, ControllerStatus::contextMismatch};
    }

    backend::ReachabilityQuery query{};
    query.start = {slot.binding.navigationScene, slot.binding.bubble, body.spec.transform.position};
    query.goal = {
        slot.binding.navigationScene, slot.binding.bubble, to_backend(command.destination)};
    query.agent = slot.binding.navigationAgent;
    backend::NavigationPath path{};
    const backend::NavigationStatus navigationStatus = navigation_->find_path(query, path);
    if (navigationStatus != backend::NavigationStatus::success || path.count == 0
        || path.count > path.points.size()) {
        stop_with_state(slot, ControllerState::pathFailed);
        if (!append_event(index,
                          tick,
                          ControllerEventKind::pathFailed,
                          ControllerStatus::navigationFailed,
                          navigationStatus,
                          command.destination)) {
            return {handle, navigationStatus, ControllerStatus::unhealthy};
        }
        return {handle, navigationStatus, ControllerStatus::navigationFailed};
    }
    slot.path = path;
    slot.waypointIndex = 0;
    slot.commandedVelocity = {};
    slot.lastWaypointDistance = 0.0F;
    slot.stalledTicks = 0;
    slot.hasProgressSample = false;
    slot.state = ControllerState::followingPath;
    ++slot.revision;
    return {handle, navigationStatus, ControllerStatus::success};
}

} // namespace sunrise::server::gameplay::physics::controller
