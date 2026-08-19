#include <cstddef>

#include "actor_controller_service.h"
#include "controller_internal.h"

namespace sunrise::server::gameplay::physics::controller {

using namespace internal;

/**
 * Queues ordered kinematic targets for one exact fixed tick.
 * @param tick Strictly increasing host tick.
 * @param actors Immutable logical actor view for generation and bubble gates.
 * @return Bounded work and outcome counts plus service health.
 */
ControllerStepResult
ActorControllerService::step(world::TickId tick,
                             std::span<const world::ActorSnapshot> actors) noexcept {
    ControllerStepResult result{};
    if (!initialized_) {
        return result;
    }
    if (!healthy_ || tick == 0 || tick <= lastTick_ || !finite(fixedDt_) || fixedDt_ <= 0.0F) {
        result.healthy = false;
        return result;
    }
    const std::size_t eventsBefore = eventCount_;
    lastTick_ = tick;
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        Slot& slot = slots_[index];
        if (!slot.occupied) {
            continue;
        }
        ++result.controllersProcessed;

        backend::BodyState body{};
        if (physics_ == nullptr || !physics_->read_body(slot.binding.body, body)) {
            const bool changed = slot.state != ControllerState::actorStale;
            if (changed) {
                stop_with_state(slot, ControllerState::actorStale);
                static_cast<void>(append_event(index,
                                               tick,
                                               ControllerEventKind::actorStale,
                                               ControllerStatus::staleBody,
                                               backend::NavigationStatus::staleScene,
                                               {}));
            }
            continue;
        }
        const backend::Vec3 position = body.spec.transform.position;
        const world::ActorSnapshot* actor = find_actor(actors, slot.binding.actor);
        if (actor == nullptr) {
            const bool changed = slot.state != ControllerState::actorStale;
            if (changed) {
                stop_with_state(slot, ControllerState::actorStale);
                static_cast<void>(append_event(index,
                                               tick,
                                               ControllerEventKind::actorStale,
                                               ControllerStatus::staleActor,
                                               backend::NavigationStatus::success,
                                               to_world(position)));
            }
            continue;
        }
        if (actor->bubble != slot.binding.bubble || body.spec.stableOwnerId != slot.binding.actor.id
            || body.spec.gate.bubble != slot.binding.bubble
            || body.spec.motion != backend::MotionKind::kinematic) {
            const bool changed = slot.state != ControllerState::contextMismatch;
            if (changed) {
                stop_with_state(slot, ControllerState::contextMismatch);
                static_cast<void>(append_event(index,
                                               tick,
                                               ControllerEventKind::contextMismatch,
                                               ControllerStatus::contextMismatch,
                                               backend::NavigationStatus::contextMismatch,
                                               to_world(position)));
            }
            continue;
        }

        const world::ActorSnapshot* facingActor = nullptr;
        if (slot.profile.facing == FacingMode::actor) {
            facingActor = find_actor(actors, slot.targetActor);
            if (facingActor == nullptr || facingActor->bubble != slot.binding.bubble) {
                const bool changed = slot.state != ControllerState::targetStale;
                if (changed) {
                    stop_with_state(slot, ControllerState::targetStale);
                    static_cast<void>(append_event(index,
                                                   tick,
                                                   ControllerEventKind::targetStale,
                                                   ControllerStatus::staleActor,
                                                   backend::NavigationStatus::success,
                                                   to_world(position)));
                }
                continue;
            }
        }
        step_controller(index, slot, body, facingActor, tick, result);
    }

    if (!healthy_) {
        for (Slot& slot : slots_) {
            if (slot.occupied) {
                static_cast<void>(stop_motion(slot));
            }
        }
    }
    result.eventsPublished = eventCount_ - eventsBefore;
    result.healthy = healthy_;
    return result;
}

} // namespace sunrise::server::gameplay::physics::controller
