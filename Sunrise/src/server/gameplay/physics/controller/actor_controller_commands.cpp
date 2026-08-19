#include <cstddef>
#include <cstdint>

#include "actor_controller_service.h"
#include "controller_internal.h"

namespace sunrise::server::gameplay::physics::controller {
namespace {

using namespace internal;

/**
 * Checks one resolved binding against its live kinematic body.
 * @param binding Exact logical and backend binding.
 * @param body Live body copied from the physics backend.
 * @return True when owner, motion, bubble, and query identity agree.
 */
[[nodiscard]] bool valid_binding(const ControllerBinding& binding,
                                 const backend::BodyState& body) noexcept {
    return actor_valid(binding.actor) && body.spec.stableOwnerId == binding.actor.id
           && body.spec.motion == backend::MotionKind::kinematic
           && body.spec.gate.bubble == binding.bubble
           && binding.navigationAgent.stableOwnerId == binding.actor.id
           && binding.navigationAgent.shapeId != 0
           && binding.navigationAgent.gate.bubble == binding.bubble
           && binding.navigationAgent.gate.layer < backend::kCollisionLayerCount
           && binding.navigationAgent.ignoreOwnerId == binding.actor.id;
}

} // namespace

/**
 * Applies one typed controller command after host data resolution.
 * @param command Policy command with logical targets.
 * @param binding Exact logical and backend binding.
 * @param profile Versioned generic mechanics limits.
 * @return A committed handle or a bounded rejection.
 */
ControllerOperationResult
ActorControllerService::configure_controller(const world::ConfigureControllerCommand& command,
                                             const ControllerBinding& binding,
                                             const ControllerProfile& profile) noexcept {
    if (!initialized_ || physics_ == nullptr || navigation_ == nullptr) {
        return {{}, backend::NavigationStatus::success, ControllerStatus::notInitialized};
    }
    if (!healthy_) {
        return {{}, backend::NavigationStatus::success, ControllerStatus::unhealthy};
    }
    if (!actor_equal(command.actor, binding.actor) || !valid_profile(profile)
        || !blueprint_equal(command.controller, profile.controller)
        || !finite(command.targetPosition)) {
        return {{}, backend::NavigationStatus::success, ControllerStatus::invalid};
    }
    if (profile.facing == FacingMode::actor
        && (!actor_valid(command.targetActor) || actor_equal(command.actor, command.targetActor))) {
        return {{}, backend::NavigationStatus::success, ControllerStatus::invalid};
    }
    backend::BodyState body{};
    if (!physics_->read_body(binding.body, body)) {
        return {{}, backend::NavigationStatus::success, ControllerStatus::staleBody};
    }
    if (!valid_binding(binding, body)) {
        const ControllerStatus status = body.spec.motion == backend::MotionKind::kinematic
                                            ? ControllerStatus::contextMismatch
                                            : ControllerStatus::unsupportedBody;
        return {{}, backend::NavigationStatus::success, status};
    }

    std::size_t existing = slots_.size();
    std::size_t freeSlot = slots_.size();
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        const Slot& slot = slots_[index];
        if (slot.occupied && slot.binding.actor.id == binding.actor.id) {
            if (!actor_equal(slot.binding.actor, binding.actor)) {
                return {{}, backend::NavigationStatus::success, ControllerStatus::staleActor};
            }
            existing = index;
            break;
        }
        if (!slot.occupied && generation_reusable(slot.generation) && freeSlot == slots_.size()) {
            freeSlot = index;
        }
    }
    const std::size_t index = existing != slots_.size() ? existing : freeSlot;
    if (index == slots_.size()) {
        return {{}, backend::NavigationStatus::success, ControllerStatus::full};
    }

    Slot& slot = slots_[index];
    const std::uint32_t generation = slot.generation;
    const std::uint64_t revision = slot.occupied ? slot.revision + 1U : 1U;
    if (slot.occupied) {
        static_cast<void>(stop_motion(slot));
    }
    slot = {};
    slot.generation = generation;
    slot.revision = revision;
    slot.binding = binding;
    slot.profile = profile;
    slot.targetActor = command.targetActor;
    slot.facingPosition = command.targetPosition;
    slot.state = ControllerState::idle;
    slot.occupied = true;
    static_cast<void>(stop_motion(slot));
    if (existing == slots_.size()) {
        ++controllerCount_;
    }
    return {{static_cast<std::uint16_t>(index), generation},
            backend::NavigationStatus::success,
            ControllerStatus::success};
}

/**
 * Stops and removes one exact controller generation.
 * @param controller Generation-checked controller handle.
 * @return Success or a stale/not-initialized rejection.
 */
ControllerOperationResult
ActorControllerService::remove_controller(ControllerHandle controller) noexcept {
    if (!initialized_) {
        return {controller, backend::NavigationStatus::success, ControllerStatus::notInitialized};
    }
    if (controller.slot >= slots_.size()) {
        return {controller, backend::NavigationStatus::success, ControllerStatus::staleHandle};
    }
    Slot& slot = slots_[controller.slot];
    if (!slot.occupied || slot.generation != controller.generation) {
        return {controller, backend::NavigationStatus::success, ControllerStatus::staleHandle};
    }
    static_cast<void>(stop_motion(slot));
    const std::uint32_t next = next_generation(slot.generation);
    const std::uint32_t generation = next == 0 ? slot.generation : next;
    slot = {};
    slot.generation = generation;
    --controllerCount_;
    return {controller, backend::NavigationStatus::success, ControllerStatus::success};
}

/**
 * Finds one exact logical actor in fixed controller storage.
 * @param actor Logical actor id and generation.
 * @param controller Receives the live controller handle.
 * @return True when the exact actor is configured.
 */
bool ActorControllerService::find_controller(world::ActorKey actor,
                                             ControllerHandle& controller) const noexcept {
    if (!initialized_) {
        return false;
    }
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        const Slot& slot = slots_[index];
        if (!slot.occupied || !actor_equal(slot.binding.actor, actor)) {
            continue;
        }
        controller = {static_cast<std::uint16_t>(index), slot.generation};
        return true;
    }
    return false;
}

/**
 * Copies one exact controller without exposing fixed storage.
 * @param controller Generation-checked controller handle.
 * @param snapshot Receives immutable generic state.
 * @return True when the exact controller is live.
 */
bool ActorControllerService::read_controller(ControllerHandle controller,
                                             ControllerSnapshot& snapshot) const noexcept {
    if (!initialized_ || controller.slot >= slots_.size()) {
        return false;
    }
    const Slot& slot = slots_[controller.slot];
    if (!slot.occupied || slot.generation != controller.generation) {
        return false;
    }
    ControllerSnapshot next{};
    next.handle = {controller.slot, slot.generation};
    next.binding = slot.binding;
    next.profile = slot.profile;
    next.targetActor = slot.targetActor;
    next.facingPosition = slot.facingPosition;
    next.path = slot.path;
    next.commandedVelocity = slot.commandedVelocity;
    next.lastWaypointDistance = slot.lastWaypointDistance;
    next.waypointIndex = slot.waypointIndex;
    next.revision = slot.revision;
    next.stalledTicks = slot.stalledTicks;
    next.state = slot.state;
    next.hasProgressSample = slot.hasProgressSample;
    snapshot = next;
    return true;
}

} // namespace sunrise::server::gameplay::physics::controller
